// order_client.cpp
#include <iostream>
#include <string>
#include <sstream>  // 添加这个头文件以使用 std::istringstream
#include <thread>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>

#pragma comment(lib, "ws2_32.lib")

class OrderClient
{
private:
	SOCKET client_socket;
	std::atomic<bool> connected;
	std::thread receive_thread;

public:
	OrderClient() : connected(false), client_socket(INVALID_SOCKET)
	{
		WSADATA wsaData;
		if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
		{
			throw std::runtime_error("WSAStartup failed");
		}
	}

	~OrderClient()
	{
		disconnect();
		WSACleanup();
	}

	bool connect_to_server(const std::string& host, int port)
	{
		client_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (client_socket == INVALID_SOCKET)
		{
			std::cerr << "Socket creation failed: " << WSAGetLastError() << std::endl;
			return false;
		}

		sockaddr_in server_addr;
		server_addr.sin_family = AF_INET;
		server_addr.sin_port = htons(port);

		// 使用 inet_pton 而不是已弃用的 inet_addr
		if (inet_pton(AF_INET, host.c_str(), &server_addr.sin_addr) <= 0)
		{
			std::cerr << "Invalid address/Address not supported: " << host << std::endl;
			closesocket(client_socket);
			return false;
		}

		if (connect(client_socket, (sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR)
		{
			std::cerr << "Connection failed: " << WSAGetLastError() << std::endl;
			closesocket(client_socket);
			client_socket = INVALID_SOCKET;
			return false;
		}

		connected = true;

		// 启动接收线程
		receive_thread = std::thread(&OrderClient::receive_messages, this);

		return true;
	}

	void disconnect()
	{
		connected = false;
		if (client_socket != INVALID_SOCKET)
		{
			shutdown(client_socket, SD_BOTH);
			closesocket(client_socket);
			client_socket = INVALID_SOCKET;
		}

		if (receive_thread.joinable())
		{
			receive_thread.join();
		}
	}

	void send_order(const std::string& order_type, int quantity, double price)
	{
		if (!connected)
		{
			std::cout << "Not connected to server" << std::endl;
			return;
		}

		std::string message = order_type + " " + std::to_string(quantity) + " " + std::to_string(price);
		if (send(client_socket, message.c_str(), static_cast<int>(message.length()), 0) == SOCKET_ERROR)
		{
			std::cerr << "Send failed: " << WSAGetLastError() << std::endl;
			disconnect();
		}
	}

	void cancel_order(int order_id)
	{
		if (!connected)
		{
			std::cout << "Not connected to server" << std::endl;
			return;
		}

		std::string message = "CANCEL " + std::to_string(order_id);
		if (send(client_socket, message.c_str(), static_cast<int>(message.length()), 0) == SOCKET_ERROR)
		{
			std::cerr << "Send failed: " << WSAGetLastError() << std::endl;
			disconnect();
		}
	}

	void request_status()
	{
		if (!connected)
		{
			std::cout << "Not connected to server" << std::endl;
			return;
		}

		std::string message = "STATUS";
		if (send(client_socket, message.c_str(), static_cast<int>(message.length()), 0) == SOCKET_ERROR)
		{
			std::cerr << "Send failed: " << WSAGetLastError() << std::endl;
			disconnect();
		}
	}

private:
	void receive_messages()
	{
		char buffer[1024];
		int bytes_received;

		while (connected)
		{
			bytes_received = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
			if (bytes_received == SOCKET_ERROR)
			{
				if (WSAGetLastError() != WSAEWOULDBLOCK)
				{
					std::cout << "Receive error: " << WSAGetLastError() << std::endl;
					connected = false;
				}
				continue;
			}
			else if (bytes_received == 0)
			{
				std::cout << "Server closed the connection" << std::endl;
				connected = false;
				break;
			}

			buffer[bytes_received] = '\0';
			process_message(std::string(buffer));
		}
	}

	void process_message(const std::string& message)
	{
		std::cout << "Server: " << message << std::endl;
	}
};

int main()
{
	try
	{
		OrderClient client;

		// 连接到服务器
		std::cout << "Connecting to server..." << std::endl;
		if (!client.connect_to_server("127.0.0.1", 12345))
		{
			std::cerr << "Failed to connect to server" << std::endl;
			return 1;
		}

		std::cout << "Connected to server. Enter commands:" << std::endl;
		std::cout << "  BUY <quantity> <price>" << std::endl;
		std::cout << "  SELL <quantity> <price>" << std::endl;
		std::cout << "  CANCEL <order_id>" << std::endl;
		std::cout << "  STATUS" << std::endl;
		std::cout << "  EXIT" << std::endl;

		std::string command;
		while (true)
		{
			std::cout << "> ";
			std::getline(std::cin, command);

			if (command == "EXIT")
			{
				break;
			}

			std::istringstream iss(command);
			std::string cmd;
			iss >> cmd;

			if (cmd == "BUY" || cmd == "SELL")
			{
				int quantity, price;
				if (iss >> quantity >> price)
				{
					client.send_order(cmd, quantity, price);
				}
				else
				{
					std::cout << "Invalid syntax. Use: " << cmd << " quantity price" << std::endl;
				}
			}
			else if (cmd == "CANCEL")
			{
				int order_id;
				if (iss >> order_id)
				{
					client.cancel_order(order_id);
				}
				else
				{
					std::cout << "Invalid syntax. Use: CANCEL order_id" << std::endl;
				}
			}
			else if (cmd == "STATUS")
			{
				client.request_status();
			}
			else if (!cmd.empty())
			{
				std::cout << "Unknown command: " << cmd << std::endl;
			}
		}

		client.disconnect();
	}
	catch (const std::exception& e)
	{
		std::cerr << "Error: " << e.what() << std::endl;
		return 1;
	}

	return 0;
}