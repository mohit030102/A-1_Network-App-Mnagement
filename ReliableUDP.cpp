/*
	Reliability and Flow Control Example
	From "Networking for Game Programmers" - http://www.gaffer.org/networking-for-game-programmers
	Author: Glenn Fiedler <gaffer@gaffer.org>
*/
#pragma warning(disable:4996)
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <fstream>
#include <sys/stat.h>
#include <cstdio>
#include <ctime>
#include <cstring>
#include "Net.h"
struct FileMetadata
{
	std::string fileName;
	std::string fileType;
	size_t fileSize;
	std::string creationDate;
	std::string modificationDate;
	std::string accessDate;
	std::string owner;
	std::string permissions;
	std::string keywords;
	std::string checksum;
	std::string filePath;
};


//#define SHOW_ACKS

using namespace std;
using namespace net;

const int ServerPort = 30000;
const int ClientPort = 30001;
const int ProtocolId = 0x11223344;
const float DeltaTime = 1.0f / 30.0f;
const float SendRate = 1.0f / 30.0f;
const float TimeOut = 10.0f;
const int PacketSize = 256;

class FlowControl
{
public:

	FlowControl()
	{
		printf("flow control initialized\n");
		Reset();
	}

	void Reset()
	{
		mode = Bad;
		penalty_time = 4.0f;
		good_conditions_time = 0.0f;
		penalty_reduction_accumulator = 0.0f;
	}

	void Update(float deltaTime, float rtt)
	{
		const float RTT_Threshold = 250.0f;

		if (mode == Good)
		{
			if (rtt > RTT_Threshold)
			{
				printf("*** dropping to bad mode ***\n");
				mode = Bad;
				if (good_conditions_time < 10.0f && penalty_time < 60.0f)
				{
					penalty_time *= 2.0f;
					if (penalty_time > 60.0f)
						penalty_time = 60.0f;
					printf("penalty time increased to %.1f\n", penalty_time);
				}
				good_conditions_time = 0.0f;
				penalty_reduction_accumulator = 0.0f;
				return;
			}

			good_conditions_time += deltaTime;
			penalty_reduction_accumulator += deltaTime;

			if (penalty_reduction_accumulator > 10.0f && penalty_time > 1.0f)
			{
				penalty_time /= 2.0f;
				if (penalty_time < 1.0f)
					penalty_time = 1.0f;
				printf("penalty time reduced to %.1f\n", penalty_time);
				penalty_reduction_accumulator = 0.0f;
			}
		}

		if (mode == Bad)
		{
			if (rtt <= RTT_Threshold)
				good_conditions_time += deltaTime;
			else
				good_conditions_time = 0.0f;

			if (good_conditions_time > penalty_time)
			{
				printf("*** upgrading to good mode ***\n");
				good_conditions_time = 0.0f;
				penalty_reduction_accumulator = 0.0f;
				mode = Good;
				return;
			}
		}
	}

	float GetSendRate()
	{
		return mode == Good ? 30.0f : 10.0f;
	}

private:

	enum Mode
	{
		Good,
		Bad
	};

	Mode mode;
	float penalty_time;
	float good_conditions_time;
	float penalty_reduction_accumulator;
};

/**/

////////////////////////////////////////////
// Simple checksum calculation
unsigned long CalculateChecksum(const std::string& filePath)
{
	FILE* file = fopen(filePath.c_str(), "rb");
	if (!file)
		return 0;

	unsigned long checksum = 0;
	char buffer[1024];
	size_t bytesRead;

	while ((bytesRead = fread(buffer, 1, sizeof(buffer), file)) > 0)
	{
		for (size_t i = 0; i < bytesRead; ++i)
		{
			checksum += buffer[i];
		}
	}

	fclose(file);
	return checksum;
}

void PrintFileMetadata(const std::string& filePath)
{
	struct stat fileStat;

	if (stat(filePath.c_str(), &fileStat) != 0)
	{
		printf("Error: Could not retrieve metadata for file: %s\n", filePath.c_str());
		return;
	}

	// File size
	size_t fileSize = fileStat.st_size;

	// Modification time
	char modTime[20];
	strftime(modTime, sizeof(modTime), "%Y-%m-%d %H:%M:%S", localtime(&fileStat.st_mtime));

#ifdef _WIN32
	// Windows-specific check for directory
	const char* fileType = (fileStat.st_mode & _S_IFDIR) ? "Directory" : "Regular File";
#else
	// POSIX check for directory
	const char* fileType = S_ISDIR(fileStat.st_mode) ? "Directory" : "Regular File";
#endif

	// Simple checksum
	unsigned long checksum = CalculateChecksum(filePath);

	// Print metadata
	printf("File Metadata:\n");
	printf("  File Path: %s\n", filePath.c_str());
	printf("  File Type: %s\n", fileType);
	printf("  File Size: %zu bytes\n", fileSize);
	printf("  Modification Date: %s\n", modTime);
	printf("  Checksum (Simple): %lu\n", checksum);
}


void SendFile(ReliableConnection& connection, const std::string& filePath) {
	// Extracting the name of file from the path
	std::string fileName = filepath.substr(filePath.find_last_of("/ \\") + 1);

	// Sending the name of the file
	connection.SendPacket(reinterpret_cast<const unsigned char*>(fileName.c_str()), fileName.size() + 1);    // include the null terminator

	// Opening the file in binary mode
	std::ifstream file(filePath, std::ios::binary);
	if (!file)
	{
		printf("Unable to open the file!! %s\n", filepath.c_str());
		return;
	}

	// Sending the data of the file.
	char buffer[PacketSize];
	while (file.read(buffer, sizeof(buffer)) || file.gcount() > 0)
	{
		connection.SendPacket(reinterpret_cast<const unsigned char*>(buffer), file.gcount());
	}

	file.close();
	printf("Binary file %s has been sent successfully.\n", filepath.c_str());
}

void ReceiveFile(ReliableConnection& connection, const std::string& destinationPath) {
	std::ofstream file(destinationPath, std::ios::binary);
	if (!file) {
		printf("Failed to open file for writing: %s\n", destinationPath.c_str());
		return;
	}

	uint32_t expectedSequence = 0;
	unsigned char buffer[PacketSize];

	while (true) {
		int bytesReceived = connection.ReceivePacket(buffer, sizeof(buffer));

		if (bytesReceived > 0) {
			// Check for termination packet
			if (bytesReceived == sizeof(uint32_t) &&
				memcmp(buffer, "\xFF\xFF\xFF\xFF", sizeof(uint32_t)) == 0) {
				printf("Received termination packet. File transfer complete.\n");
				break;
			}

			// Extract sequence number and data
			uint32_t receivedSequence;
			memcpy(&receivedSequence, buffer, sizeof(receivedSequence));

			if (receivedSequence == expectedSequence) {
				file.write(reinterpret_cast<char*>(buffer + sizeof(receivedSequence)),
					bytesReceived - sizeof(receivedSequence));
				//printf("Received and wrote packet %u (%d bytes)\n", receivedSequence, bytesReceived - sizeof(receivedSequence));

				expectedSequence++;

				// Send acknowledgment for the received packet
				connection.SendPacket(reinterpret_cast<unsigned char*>(&receivedSequence), sizeof(receivedSequence));
			}
			else {
				printf("Out-of-order packet %u received. Expected %u. Ignored.\n", receivedSequence, expectedSequence);
			}
		}
	}

	file.close();
	printf("File received successfully and saved to %s\n", destinationPath.c_str());
}






////////////////////////////////////////////////////////


// ----------------------------------------------

int main(int argc, char* argv[])
{
	// parse command line
	enum Mode
	{
		Client,
		Server
	};

	Mode mode = Server;
	Address address;

	if (argc >= 2)
	{
		int a, b, c, d;
#pragma warning(suppress : 4996)
		if (sscanf(argv[1], "%d.%d.%d.%d", &a, &b, &c, &d))
		{
			mode = Client;
			address = Address(a, b, c, d, ServerPort);
		}
	}

	// Get The File
	std::string filePath;

	if (mode == Client)
	{
		try
		{
			if (argc < 3)
			{
				throw std::runtime_error("File path not provided. Usage: <program> <server_ip> <file_path>");
			}
			filePath = argv[2];
			printf("File path provided: %s\n", filePath.c_str());
			PrintFileMetadata(filePath);
		}
		catch (const std::exception& e)
		{
			printf("Error: %s\n", e.what());
			return 1;
		}
	}

	// initialize
	if (!InitializeSockets())
	{
		printf("failed to initialize sockets\n");
		return 1;
	}

	ReliableConnection connection(ProtocolId, TimeOut);

	const int port = mode == Server ? ServerPort : ClientPort;

	if (!connection.Start(port))
	{
		printf("could not start connection on port %d\n", port);
		return 1;
	}

	if (mode == Client)
		connection.Connect(address);
	else
		connection.Listen();

	bool connected = false;
	float sendAccumulator = 0.0f;
	float statsAccumulator = 0.0f;

	FlowControl flowControl;

	while (true)
	{
		// update flow control
		if (connection.IsConnected())
			flowControl.Update(DeltaTime, connection.GetReliabilitySystem().GetRoundTripTime() * 1000.0f);

		const float sendRate = flowControl.GetSendRate();

		// detect changes in connection state
		if (mode == Server && connected && !connection.IsConnected())
		{
			flowControl.Reset();
			printf("reset flow control\n");
			connected = false;
		}

		if (!connected && connection.IsConnected())
		{
			printf("client connected to server\n");
			connected = true;
		}

		if (!connected && connection.ConnectFailed())
		{
			printf("connection failed\n");
			break;
		}

		// send and receive packets
		sendAccumulator += DeltaTime;

		while (sendAccumulator > 1.0f / sendRate)
		{
			unsigned char packet[PacketSize];
			memset(packet, 0, sizeof(packet));
			connection.SendPacket(packet, sizeof(packet));
			sendAccumulator -= 1.0f / sendRate;
		}

		while (true)
		{
			unsigned char packet[256];
			int bytes_read = connection.ReceivePacket(packet, sizeof(packet));
			if (bytes_read == 0)
				break;
		}

		// show packets that were acked this frame
#ifdef SHOW_ACKS
		unsigned int* acks = NULL;
		int ack_count = 0;
		connection.GetReliabilitySystem().GetAcks(&acks, ack_count);
		if (ack_count > 0)
		{
			printf("acks: %d", acks[0]);
			for (int i = 1; i < ack_count; ++i)
				printf(",%d", acks[i]);
			printf("\n");
		}
#endif

		// update connection
		connection.Update(DeltaTime);

		// show connection stats
		statsAccumulator += DeltaTime;

		while (statsAccumulator >= 0.25f && connection.IsConnected())
		{
			float rtt = connection.GetReliabilitySystem().GetRoundTripTime();
			unsigned int sent_packets = connection.GetReliabilitySystem().GetSentPackets();
			unsigned int acked_packets = connection.GetReliabilitySystem().GetAckedPackets();
			unsigned int lost_packets = connection.GetReliabilitySystem().GetLostPackets();
			float sent_bandwidth = connection.GetReliabilitySystem().GetSentBandwidth();
			float acked_bandwidth = connection.GetReliabilitySystem().GetAckedBandwidth();

			printf("rtt %.1fms, sent %d, acked %d, lost %d (%.1f%%), sent bandwidth = %.1fkbps, acked bandwidth = %.1fkbps\n",
				rtt * 1000.0f, sent_packets, acked_packets, lost_packets,
				sent_packets > 0.0f ? (float)lost_packets / (float)sent_packets * 100.0f : 0.0f,
				sent_bandwidth, acked_bandwidth);

			statsAccumulator -= 0.25f;
		}

		net::wait(DeltaTime);

		// Add the file sending/receiving logic here:
		if (mode == Server && connected)
		{
			std::string filePath = "C:/Users/amala/Desktop/file.txt";  // Change this path to the file you want to send
			SendFile(connection, filePath);
		}

		if (mode == Client && connected)
		{
			std::string desktopPath = "D:/Assignment Level-4/SENG2040-Network Application Development/received_file.txt";  // Change this to your desired file name
			ReceiveFile(connection, desktopPath);
		}

	}

	ShutdownSockets();

	return 0;
}

