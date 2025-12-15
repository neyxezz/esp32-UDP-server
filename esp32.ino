#include <WiFi.h>
#include <WiFiUdp.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

#define RGB_BUILTIN 48
#define BUTTON_BOOT 0

#define WIFI_SSID "TP-Link_F989"
#define WIFI_PASSWORD "57022697"

#define UDP_PORT 8888

WiFiUDP Udp;

uint8_t Buffer[1400];

enum
{
	C_PING,
	S_REPLY,
	C_INFO,
	S_INFO_SUCCESS,
	C_SAY,
	S_CHAT,
	C_DISCONNECT,
	S_DISCONNECT
};

enum
{
	STATE_OFFLINE,
	STATE_ONLINE
};

int STATE = STATE_OFFLINE;

constexpr int MAX_CLIENTS = 128;

struct ClientInfo
{
	IPAddress Addr;
	uint16_t Port;
	char Name[16];
	bool Free = true;
	unsigned long LastAcked;
};

ClientInfo Clients[MAX_CLIENTS];

void ConnectWiFi()
{
	WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

	Serial.print("Подключение к WiFi");

	unsigned long startTime = millis();
	while(WiFi.status() != WL_CONNECTED)
	{
		neopixelWrite(RGB_BUILTIN, 1, 0, 0);
		delay(250);
		neopixelWrite(RGB_BUILTIN, 0, 0, 0);
		delay(250);

		Serial.print(".");

		if(millis() - startTime > 10000)
		{
			Serial.println("\nТаймаут подключения!");
			ESP.restart();
		}
	}

	neopixelWrite(RGB_BUILTIN, 0, 1, 0);

	Serial.println("\nПодключено к WiFi!");
	Serial.print("IP: ");
	Serial.println(WiFi.localIP());
}

class MsgPacker
{
private:
	static const size_t BufferSize = 1400;
	uint8_t InternalBuffer[BufferSize];
	size_t Position;

public:
	MsgPacker(int32_t Msg) :
		Position(0)
	{
		memset(InternalBuffer, 0, BufferSize);
		AddInt(Msg);
	}

	bool AddInt(int32_t Value)
	{
		if(Position + sizeof(int32_t) > BufferSize)
			return false;

		memcpy(InternalBuffer + Position, &Value, sizeof(int32_t));
		Position += sizeof(int32_t);
		return true;
	}

	bool AddString(const char *Str)
	{
		if(Str == nullptr || Position >= BufferSize)
			return false;

		size_t StrLen = strlen(Str) + 1;

		if(Position + StrLen > BufferSize)
			return false;

		memcpy(InternalBuffer + Position, Str, StrLen);
		Position += StrLen;
		return true;
	}

	operator const uint8_t *() const
	{
		return InternalBuffer;
	}

	operator size_t () const
	{
		return Position;
	}
};

class MsgUnpacker
{
private:
	const uint8_t *BufferPtr;
	size_t Position;
	size_t BufferSize;

public:
	MsgUnpacker(const uint8_t *buffer, size_t bufferSize)
	{
		BufferPtr = buffer;
		BufferSize = bufferSize;
		Position = 0;
	}

	int32_t UnpackInt()
	{
		if(Position + sizeof(int32_t) > BufferSize)
		{
			return 0;
		}

		int32_t value;
		memcpy(&value, BufferPtr + Position, sizeof(int32_t));
		Position += sizeof(int32_t);
		return value;
	}

	const char *UnpackString()
	{
		if(Position >= BufferSize)
		{
			return nullptr;
		}

		const char *strPtr = reinterpret_cast<const char *>(BufferPtr + Position);
		size_t remaining = BufferSize - Position;

		for(size_t i = 0; i < remaining; i++)
		{
			if(BufferPtr[Position + i] == 0)
			{
				Position += i + 1;
				return strPtr;
			}
		}

		return nullptr;
	}
};

void SendPacket(auto &Packer, IPAddress Addr, uint16_t Port)
{
	Udp.beginPacket(Addr, Port);
	Udp.write(Packer, Packer);
	Udp.endPacket();
}

int GetFreeSlot()
{
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(Clients[i].Free)
			return i;
	}
	return -1;
}

int FindClientByAddress(IPAddress Addr, uint16_t Port)
{
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(!Clients[i].Free && Clients[i].Addr == Addr && Clients[i].Port == Port)
			return i;
	}
	return -1;
}

int FindClientByName(const char *Name)
{
	if(Name == nullptr) return -1;
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(!Clients[i].Free && !strcmp(Clients[i].Name, Name))
		{
			return i;
		}
	}
	return -1;
}

/*int GetSlot(IPAddress Addr, uint16_t Port)
{
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(Clients[i].Free || Addr[0] != Clients[i].Addr[0] || Addr[1] != Clients[i].Addr[1] || Addr[2] != Clients[i].Addr[2] || Addr[3] != Clients[i].Addr[3] ||
			Port != Clients[i].Port)
			continue;
		return i;
	}
	return -1;
}

int GetLatestSlot()
{
	for(int i = MAX_CLIENTS; i >= 0; i--)
	{
		if(!Clients[i].Free)
			return i;
	}
	return -1;
}*/

void ClearSlot(int Slot)
{
	Clients[Slot].Free = true;
	Clients[Slot].Addr = IPAddress(0, 0, 0, 0);
	Clients[Slot].Port = 0;
	memset(Clients[Slot].Name, 0, sizeof(Clients[Slot].Name));
	Clients[Slot].LastAcked = 0;
}

void ProcessPacket(IPAddress Addr, uint16_t Port)
{
	int BytesRead = Udp.read(Buffer, sizeof(Buffer));

	if(BytesRead > 0)
	{
		MsgUnpacker Unpacker(Buffer, BytesRead);

		int32_t Msg = Unpacker.UnpackInt();

		if(Msg == C_INFO)
		{
			int ExistSlot = FindClientByAddress(Addr, Port);
			if(ExistSlot >= 0)
				return;

			int FreeSlot = GetFreeSlot();
			if(FreeSlot == -1)
			{
				MsgPacker Packer(S_DISCONNECT);
				Packer.AddString("Full server");
				SendPacket(Packer, Addr, Port);
			}

			const char *Name = Unpacker.UnpackString();
			if(Name == nullptr || strlen(Name) == 0)
				return;

			if(FindClientByName(Name) >= 0)
			{
				MsgPacker Packer(S_DISCONNECT);
				Packer.AddString("Name already exists");
				SendPacket(Packer, Addr, Port);
				return;
			}

			Clients[FreeSlot].Free = false;
			Clients[FreeSlot].Addr = Addr;
			Clients[FreeSlot].Port = Port;
			strncpy(Clients[FreeSlot].Name, Name, 16);
			Clients[FreeSlot].LastAcked = millis();

			// send success to client
			{
				MsgPacker Packer(S_INFO_SUCCESS);
				SendPacket(Packer, Addr, Port);
			}

			// send to all clients
			{
				char Buf[32];
				snprintf(Buf, sizeof(Buf), "%s connected", Clients[FreeSlot].Name);

				MsgPacker Packer(S_CHAT);
				Packer.AddInt(1);
				Packer.AddString(Buf);
				for(int i = 0; i < MAX_CLIENTS; i++)
				{
					if(!Clients[i].Free)
						SendPacket(Packer, Clients[i].Addr, Clients[i].Port);
				}
			}
		}
		if(Msg == C_PING)
		{
			int Slot = FindClientByAddress(Addr, Port);
			if(Slot == -1)
			{
				return;
			}
			Clients[Slot].LastAcked = millis();
			MsgPacker Packer(S_REPLY);

			SendPacket(Packer, Addr, Port);
		}
		if(Msg == C_SAY)
		{
			const char *Message = Unpacker.UnpackString();
			int Slot = FindClientByAddress(Addr, Port);
			if(Slot == -1)
			{
				return;
			}

			MsgPacker Packer(S_CHAT);
			Packer.AddInt(0);
			Packer.AddString(Clients[Slot].Name);
			Packer.AddString(Message);

			for(int i = 0; i < MAX_CLIENTS; i++)
			{
				if(!Clients[i].Free)
					SendPacket(Packer, Clients[i].Addr, Clients[i].Port);
			}
		}
		if(Msg == C_DISCONNECT)
		{
			int Slot = FindClientByAddress(Addr, Port);
			if(Slot == -1)
			{
				return;
			}

			char Buf[32];
			snprintf(Buf, sizeof(Buf), "%s disconnected", Clients[Slot].Name);

			MsgPacker Packer(S_CHAT);
			Packer.AddInt(1);
			Packer.AddString(Buf);

			for(int i = 0; i < MAX_CLIENTS; i++)
			{
				if(!Clients[i].Free)
					SendPacket(Packer, Clients[i].Addr, Clients[i].Port);
			}

			ClearSlot(Slot);
		}
	}
}

void setup()
{
	Serial.begin(115200);

	pinMode(RGB_BUILTIN, OUTPUT);
	pinMode(BUTTON_BOOT, INPUT_PULLUP);

	neopixelWrite(RGB_BUILTIN, 1, 0, 0);

	Serial.println("Starting ESP32-S3 UDP Server");

	ConnectWiFi();

	if(Udp.begin(UDP_PORT))
	{
		Serial.print("UDP server started on port ");
		Serial.println(UDP_PORT);
	}
	else
	{
		Serial.println("failed");
		ESP.restart();
	}
}

void loop()
{
	int PacketSize = Udp.parsePacket();

	if(PacketSize > 0)
	{
		IPAddress RemoteIP = Udp.remoteIP();
		uint16_t RemotePort = Udp.remotePort();
		Serial.print("Received packet from ");
		Serial.print(RemoteIP);
		Serial.print(":");
		Serial.print(RemotePort);
		Serial.print(", size: ");
		Serial.println(PacketSize);

		ProcessPacket(RemoteIP, RemotePort);
	}

	// handle not acked clients
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(!Clients[i].Free && (millis() - Clients[i].LastAcked > 10000))
		{
			ClearSlot(i);
		}
	}
}
