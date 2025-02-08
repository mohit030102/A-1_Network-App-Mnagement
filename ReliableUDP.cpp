#define WIDTH (8 * sizeof(crc))
#define TOPBIT (1 << (WIDTH - 1))
#define POLYNOMIAL 0x07
#define NOMINMAX
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
#include <sys/stat.h>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <chrono>

#include "Net.h"
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

/*
Reference - https://barrgroup.com/blog/crc-series-part-3-crc-implementation-code-cc
*/
typedef uint8_t crc;  // Define CRC as 8-bit
crc crcCalc(uint8_t const message[], int nBytes)
{
	crc remainder = 0;

	for (int byte = 0; byte < nBytes; ++byte)
	{
		remainder ^= (message[byte] << (WIDTH - 8));
		for (uint8_t bit = 8; bit > 0; --bit)
		{
			if (remainder & TOPBIT)
			{
				remainder = (remainder << 1) ^ POLYNOMIAL;
			}
			else
			{
				remainder = (remainder << 1);
			}
		}
	}
	return remainder;
}




void SendIt(ReliableConnection& connection, const std::string& filePath) {
	using namespace std::chrono;
	// Extracting the name of file from the path
	std::string fileName = filePath.substr(filePath.find_last_of("/\\") + 1);

	// Sending the name of the file
	connection.SendPacket(reinterpret_cast<const unsigned char*>(fileName.c_str()), fileName.size() + 1);    // include the null terminator

	// Opening the file in binary mode
	std::ifstream file(filePath, std::ios::binary);
	if (!file)
	{
		printf("Unable to open the file!! %s\n", filePath.c_str());
		return;
	}
	// Read content for CRC calculation
	std::vector<uint8_t> fileContent((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	file.close();

	crc checksum = crcCalc(fileContent.data(), fileContent.size());
	printf("CRC for file: 0x%02X\n", checksum);
	// Start timing
	auto start = high_resolution_clock::now();
	// Send file content
	for (size_t i = 0; i < fileContent.size(); i += PacketSize)
	{
		size_t chunkSize = (PacketSize < (fileContent.size() - i)) ? PacketSize : (fileContent.size() - i);
		connection.SendPacket(fileContent.data() + i, chunkSize);
	}

	// Send CRC as final packet
	connection.SendPacket(reinterpret_cast<const unsigned char*>(&checksum), sizeof(checksum));
	// End timing
	auto end = high_resolution_clock::now();

	// Calculate transmission time
	duration<double> timeTook = end - start;
	double inSeconds = timeTook.count();

	// Calculate transfer speed in megabits per second
	double fileSizeInMegabits = (fileContent.size() * 8) / (1024.0 * 1024.0); // Convert bytes to megabits
	double speedMbps = fileSizeInMegabits / inSeconds;

	printf("File %s sent with CRC 0x%02X.\n", filePath.c_str(), checksum);
	printf("Transmission Time: %.2f seconds\n", inSeconds);
	printf("Transfer Speed: %.2f Mbps\n", speedMbps);
}

void ReceiveIt(ReliableConnection& connection) 
{
	char fileNameBuffer[256] = { 0 };
	int bytesRead = connection.ReceivePacket(reinterpret_cast<unsigned char*>(fileNameBuffer), sizeof(fileNameBuffer));

	if (bytesRead <= 0 || bytesRead >= sizeof(fileNameBuffer))
	{
		printf("Invalid filename received.\n");
		return;
	}

	std::string fileName(fileNameBuffer, bytesRead);
	if (fileName.empty() || fileName.find_first_of("\\/:*?\"<>|") != std::string::npos)
	{
		printf("Invalid filename received.\n");
		return;
	}

	std::ofstream outFile(fileName, std::ios::binary);
	if (!outFile)
	{
		printf("Failed to create file: %s\n", fileName.c_str());
		return;
	}

	char buffer[1024];
	crc receivedChecksum = 0;
	crc calculatedChecksum = 0;
	bool fileReceived = false;

	while ((bytesRead = connection.ReceivePacket(reinterpret_cast<unsigned char*>(buffer), sizeof(buffer))) > 0)
	{
		if (bytesRead == 1)
		{
			// Assume the last packet contains the CRC
			receivedChecksum = static_cast<crc>(buffer[0]);
			fileReceived = true; // Mark file as received
			break;
		}

		calculatedChecksum ^= crcCalc(reinterpret_cast<const uint8_t*>(buffer), bytesRead);
		outFile.write(buffer, bytesRead);
	}

	outFile.close();

	if (fileReceived)
	{
		if (calculatedChecksum == receivedChecksum)
		{
			printf("File %s received successfully with valid checksum: 0x%02X\n", fileName.c_str(), receivedChecksum);
		}
		else
		{
			printf("Checksum mismatch! Received: 0x%02X, Calculated: 0x%02X\n", receivedChecksum, calculatedChecksum);
		}
	}
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

		if (mode == Server && connected)
		{
			if (argc < 2)
			{
				return 1;
			}

			std::string filePath = argv[1];
			SendIt(connection, filePath);
		}

		if (mode == Client && connected)
		{
			ReceiveIt(connection);
		}

	}

	ShutdownSockets();

	return 0;
}

