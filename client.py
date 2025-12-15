import threading
import asyncio
import select
import random
import socket
import signal
import struct
import time
import sys

C_PING = 0
S_REPLY = 1
C_INFO = 2
S_INFO_SUCCESS = 3
C_SAY = 4
S_CHAT = 5
C_DISCONNECT = 6
S_DISCONNECT = 7

STATE_OFFLINE = 0
STATE_ONLINE = 1

name = "neyxezz"

class MsgPacker:
	def __init__(self, msg, buffer_size=1400):
		self.BufferSize = buffer_size
		self.InternalBuffer = bytearray(buffer_size)
		self.Position = 0
		self.AddInt(msg);

	def AddInt(self, value):
		if self.Position + 4 > self.BufferSize:
			return False

		packed = struct.pack('<i', value)
		self.InternalBuffer[self.Position:self.Position + 4] = packed
		self.Position += 4
		return True

	def AddString(self, text):
		if text is None or self.Position >= self.BufferSize:
			return False

		text_bytes = text.encode('utf-8') + b'\x00'
		str_len = len(text_bytes)

		if self.Position + str_len > self.BufferSize:
			return False

		self.InternalBuffer[self.Position:self.Position + str_len] = text_bytes
		self.Position += str_len
		return True

	@property
	def Data(self):
		return bytes(self.InternalBuffer[:self.Position])

	@property
	def Size(self):
		return self.Position

	def Reset(self):
		self.Position = 0

	def bytes(self):
		return self.Data

	def len(self):
		return self.Size

class MsgUnpacker:
	def __init__(self, buffer):
		self.Buffer = buffer
		self.BufferSize = len(buffer)
		self.Position = 0

	def UnpackInt(self):
		if self.Position + 4 > self.BufferSize:
			return 0

		value = struct.unpack_from('<i', self.Buffer, self.Position)[0]
		self.Position += 4
		return value

	def UnpackString(self):
		if self.Position >= self.BufferSize:
			return None

		start_pos = self.Position
		null_pos = -1

		for i in range(start_pos, self.BufferSize):
			if self.Buffer[i] == 0:
				null_pos = i
				break

		if null_pos == -1:
			return None
		string_bytes = self.Buffer[start_pos:null_pos]
		self.Position = null_pos + 1

		try:
			return string_bytes.decode('utf-8')
		except UnicodeDecodeError:
			return string_bytes.decode('latin-1')

	@property
	def Remaining(self):
		return max(0, self.BufferSize - self.Position)

	def Reset(self) -> None:
		self.Position = 0

	def GetPosition(self):
		return self.Position

	def SetPosition(self, position):
		if 0 <= position <= self.BufferSize:
			self.Position = position
			return True
		return False

class Client:
	def __init__(self, host, port):
		self.host = host
		self.port = port
		self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
		self.STATE = STATE_OFFLINE
		self.receiving = True
		self.last_packet = time.time()
		self.last_ping = 0

	def send_info(self):
		packer = MsgPacker(C_INFO)
		packer.AddString(name)
		self.sock.sendto(packer.Data, (self.host, self.port))

	def send_control(self, msg):
		packer = MsgPacker(msg)
		self.sock.sendto(packer.Data, (self.host, self.port))

	def sighandler(self, _, __):
		print("disconnecting...")
		self.send_control(C_DISCONNECT)
		self.receiving = False
		sys.exit(0)

	def task(self):
		while self.receiving:
			user_input = input()
			if user_input.startswith("*"):
				cmd = user_input[1:]
				if cmd == "exit":
					self.send_control(C_DISCONNECT)
					self.receiving = False
			elif user_input:
				packer = MsgPacker(C_SAY)
				packer.AddString(user_input)
				self.sock.sendto(packer.Data, (self.host, self.port))

	async def connect(self):
		signal.signal(signal.SIGINT, self.sighandler)
		self.send_info()
		while self.receiving:
			if self.STATE == STATE_ONLINE:
				if time.time() - self.last_ping > 1:
					self.send_control(C_PING)
					self.last_ping = time.time()
			if time.time() - self.last_packet > 10:
				print("Timed out.")
				sys.exit(0)
			ready_to_read, _, _ = select.select([self.sock], [], [], 0.01)
			if not ready_to_read:
				continue
			try:
				packet, addr = self.sock.recvfrom(1400)
				if addr != (self.host, self.port):
					return

				unpacker = MsgUnpacker(packet)
				msg = unpacker.UnpackInt()

				if msg == S_INFO_SUCCESS:
					print("Connected!")
					self.STATE = STATE_ONLINE
					self.thread = threading.Thread(target=self.task, daemon=True)
					self.thread.start()

				if msg == S_REPLY:
					self.last_packet = time.time()
					print(f"Latency - {(time.time() - self.last_ping)*1000:.2f}ms")

				if msg == S_CHAT:
					Sys = unpacker.UnpackInt()
					if Sys:
						message = unpacker.UnpackString()
						print(f"*** {message}")
					else:
						name = unpacker.UnpackString()
						message = unpacker.UnpackString()
						print(f"{name}: {message}")

				if msg == S_DISCONNECT:
					reason = unpacker.UnpackString()
					print(f"disconnected. reason: {reason}")
					self.STATE = STATE_OFFLINE
					self.receiving = False
					self.sock.close()
					exit(0)
			except:
				pass
		sys.exit(0)

async def main():
	client = Client("192.168.0.212", 8888)
	await client.connect()
asyncio.run(main())
