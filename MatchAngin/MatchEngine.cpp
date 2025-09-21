// matching_engine.cpp
#include <iostream>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <sstream>
#include <atomic>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>

#pragma comment(lib, "ws2_32.lib")

// 订单类
class Order
{
public:
	int id;
	bool is_buy;
	int quantity;
	double price;
	int client_id;

	Order(int order_id, bool buy, int qty, double prc, int cli_id)
		: id(order_id), is_buy(buy), quantity(qty), price(prc), client_id(cli_id)
	{
	}
};

// 价格水平类
class PriceLevel
{
public:
	double price;
	std::vector<std::shared_ptr<Order>> orders;
	int total_quantity;

	PriceLevel(double prc) : price(prc), total_quantity(0)
	{
	}

	void add_order(std::shared_ptr<Order> order)
	{
		orders.push_back(order);
		total_quantity += order->quantity;
	}

	void remove_order(int order_id)
	{
		for (auto it = orders.begin(); it != orders.end(); ++it)
		{
			if ((*it)->id == order_id)
			{
				total_quantity -= (*it)->quantity;
				orders.erase(it);
				return;
			}
		}
		throw std::runtime_error("Order not found in PriceLevel");
	}
};

// 订单簿类
class OrderBook
{
private:
	std::unordered_set<double> bid_prices;
	std::unordered_set<double> ask_prices;
	std::unordered_map<double, std::shared_ptr<PriceLevel>> bid_price_map;
	std::unordered_map<double, std::shared_ptr<PriceLevel>> ask_price_map;
	std::unordered_map<int, std::shared_ptr<Order>> order_id_map;
	int current_order_id;
	mutable std::mutex mtx;

	// 获取最高买价
	double get_best_bid() const
	{
		if (bid_prices.empty()) return -1;
		return *std::max_element(bid_prices.begin(), bid_prices.end());
	}

	// 获取最低卖价
	double get_best_ask() const
	{
		if (ask_prices.empty()) return -1;
		return *std::min_element(ask_prices.begin(), ask_prices.end());
	}

public:
	OrderBook() : current_order_id(0)
	{
	}

	int add_order(bool is_buy, int quantity, double price, int client_id)
	{
		std::lock_guard<std::mutex> lock(mtx);
		if (quantity <= 0 || price <= 0)
		{
			throw std::invalid_argument("Quantity and price must be positive");
		}

		current_order_id++;
		auto order = std::make_shared<Order>(current_order_id, is_buy, quantity, price, client_id);
		order_id_map.emplace(order->id, order);

		if (is_buy)
		{
			if (bid_price_map.find(price) != bid_price_map.end())
			{
				bid_price_map[price]->add_order(order);
			}
			else
			{
				auto level = std::make_shared<PriceLevel>(price);
				level->add_order(order);
				bid_price_map.emplace(price, level);
				bid_prices.insert(price);
			}
		}
		else
		{
			if (ask_price_map.find(price) != ask_price_map.end())
			{
				ask_price_map[price]->add_order(order);
			}
			else
			{
				auto level = std::make_shared<PriceLevel>(price);
				level->add_order(order);
				ask_price_map.emplace(price, level);
				ask_prices.insert(price);
			}
		}

		return current_order_id;
	}

	void cancel_order(int order_id)
	{
		std::lock_guard<std::mutex> lock(mtx);
		auto it = order_id_map.find(order_id);
		if (it == order_id_map.end())
		{
			throw std::runtime_error("Order not found");
		}

		auto order = it->second;
		if (order->is_buy)
		{
			auto& level = bid_price_map[order->price];
			level->remove_order(order_id);
			if (level->total_quantity == 0)
			{
				bid_price_map.erase(order->price);
				bid_prices.erase(order->price);
			}
		}
		else
		{
			auto& level = ask_price_map[order->price];
			level->remove_order(order_id);
			if (level->total_quantity == 0)
			{
				ask_price_map.erase(order->price);
				ask_prices.erase(order->price);
			}
		}
		order_id_map.erase(it);
	}

	std::vector<std::string> execute_trades()
	{
		std::lock_guard<std::mutex> lock(mtx);
		std::vector<std::string> trade_messages;

		while (true)
		{
			double best_bid = get_best_bid();
			double best_ask = get_best_ask();

			if (best_bid == -1 || best_ask == -1 || best_bid < best_ask)
			{
				break;
			}

			auto& bid_level = bid_price_map[best_bid];
			auto& ask_level = ask_price_map[best_ask];

			if (bid_level->orders.empty() || ask_level->orders.empty())
			{
				// 清理空的价格水平
				if (bid_level->orders.empty())
				{
					bid_price_map.erase(best_bid);
					bid_prices.erase(best_bid);
				}
				if (ask_level->orders.empty())
				{
					ask_price_map.erase(best_ask);
					ask_prices.erase(best_ask);
				}
				continue;
			}

			auto& bid_order = bid_level->orders.front();
			auto& ask_order = ask_level->orders.front();
			int trade_qty = min(bid_order->quantity, ask_order->quantity);

			std::stringstream msg;
			msg << "TRADE " << bid_order->id << " " << ask_order->id << " "
				<< trade_qty << " " << best_ask;
			trade_messages.push_back(msg.str());

			// 更新订单数量
			bid_order->quantity -= trade_qty;
			ask_order->quantity -= trade_qty;
			bid_level->total_quantity -= trade_qty;
			ask_level->total_quantity -= trade_qty;

			// 移除已完全成交的订单
			if (bid_order->quantity == 0)
			{
				bid_level->orders.erase(bid_level->orders.begin());
				order_id_map.erase(bid_order->id);
			}
			if (ask_order->quantity == 0)
			{
				ask_level->orders.erase(ask_level->orders.begin());
				order_id_map.erase(ask_order->id);
			}

			// 如果价格水平为空，移除它
			if (bid_level->orders.empty())
			{
				bid_price_map.erase(best_bid);
				bid_prices.erase(best_bid);
			}
			if (ask_level->orders.empty())
			{
				ask_price_map.erase(best_ask);
				ask_prices.erase(best_ask);
			}
		}

		return trade_messages;
	}

	std::string get_order_book_string() const
	{
		std::lock_guard<std::mutex> lock(mtx);
		std::stringstream ss;

		ss << "BIDS:\n";
		std::vector<int> bid_prices_sorted(bid_prices.begin(), bid_prices.end());
		std::sort(bid_prices_sorted.rbegin(), bid_prices_sorted.rend());
		for (int price : bid_prices_sorted)
		{
			const auto& level = bid_price_map.at(price);
			ss << "  " << price << " : " << level->total_quantity << "\n";
		}

		ss << "ASKS:\n";
		std::vector<int> ask_prices_sorted(ask_prices.begin(), ask_prices.end());
		std::sort(ask_prices_sorted.begin(), ask_prices_sorted.end());
		for (int price : ask_prices_sorted)
		{
			const auto& level = ask_price_map.at(price);
			ss << "  " << price << " : " << level->total_quantity << "\n";
		}

		return ss.str();
	}

	std::string get_status() const
	{
		std::lock_guard<std::mutex> lock(mtx);
		return "Orders: " + std::to_string(order_id_map.size()) +
			", Bid levels: " + std::to_string(bid_price_map.size()) +
			", Ask levels: " + std::to_string(ask_price_map.size());

	}
};

// 客户端连接类
class ClientConnection
{
private:
	SOCKET client_socket;
	int client_id;
	std::atomic<bool> connected;
	OrderBook& order_book;

public:
	ClientConnection(SOCKET sock, int id, OrderBook& book)
		: client_socket(sock), client_id(id), connected(true), order_book(book)
	{
	}

	~ClientConnection()
	{
		if (client_socket != INVALID_SOCKET)
		{
			closesocket(client_socket);
		}
	}

	void handle_client()
	{
		char buffer[1024];
		int bytes_received;

		while (connected)
		{
			bytes_received = recv(client_socket, buffer, sizeof(buffer), 0);
			if (bytes_received <= 0)
			{
				std::cout << "Client " << client_id << " disconnected." << std::endl;
				connected = false;
				break;
			}

			buffer[bytes_received] = '\0';
			std::string message(buffer);
			process_message(message);
		}
	}

	void send_message(const std::string& message)
	{
		if (connected)
		{
			send(client_socket, message.c_str(), static_cast<int>(message.length()), 0);
		}
	}

	bool is_connected() const
	{
		return connected;
	}

	int get_client_id() const
	{
		return client_id;
	}

	SOCKET get_socket() const
	{
		return client_socket;
	}

private:
	void process_message(const std::string& message)
	{
		std::istringstream iss(message);
		std::string command;
		iss >> command;

		try
		{
			if (command == "BUY")
			{
				int quantity;
				double price;
				iss >> quantity >> price;
				int order_id = order_book.add_order(true, quantity, price, client_id);
				send_message("ORDER_ACCEPTED " + std::to_string(order_id));
			}
			else if (command == "SELL")
			{
				int quantity;
				double price;
				iss >> quantity >> price;
				int order_id = order_book.add_order(false, quantity, price, client_id);
				send_message("ORDER_ACCEPTED " + std::to_string(order_id));
			}
			else if (command == "CANCEL")
			{
				int order_id;
				iss >> order_id;
				order_book.cancel_order(order_id);
				send_message("CANCEL_ACCEPTED " + std::to_string(order_id));
			}
			else if (command == "STATUS")
			{
				send_message("STATUS " + order_book.get_status());
			}
			else
			{
				send_message("ERROR Unknown command: " + command);
			}
		}
		catch (const std::exception& e)
		{
			send_message("ERROR " + std::string(e.what()));
		}
	}
};

// 交易服务器类
class TradingServer
{
private:
	OrderBook order_book;
	SOCKET server_socket;
	std::atomic<bool> running;
	std::vector<std::unique_ptr<ClientConnection>> clients;
	std::vector<std::thread> client_threads;
	int next_client_id;
	std::thread trade_thread;

public:
	TradingServer() : running(false), next_client_id(1)
	{
		WSADATA wsaData;
		if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
		{
			throw std::runtime_error("WSAStartup failed");
		}

		server_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (server_socket == INVALID_SOCKET)
		{
			WSACleanup();
			throw std::runtime_error("Socket creation failed");
		}
	}

	~TradingServer()
	{
		stop();
		closesocket(server_socket);
		WSACleanup();
	}

	void start(int port)
	{
		sockaddr_in server_addr;
		server_addr.sin_family = AF_INET;
		server_addr.sin_addr.s_addr = INADDR_ANY;
		server_addr.sin_port = htons(port);

		if (bind(server_socket, (sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR)
		{
			throw std::runtime_error("Bind failed");
		}

		if (listen(server_socket, SOMAXCONN) == SOCKET_ERROR)
		{
			throw std::runtime_error("Listen failed");
		}

		running = true;
		std::cout << "Trading server started on port " << port << std::endl;

		// 启动交易执行线程
		trade_thread = std::thread(&TradingServer::trade_loop, this);

		// 接受客户端连接
		accept_clients();
	}

	void stop()
	{
		running = false;

		// 关闭服务器套接字
		closesocket(server_socket);
		server_socket = INVALID_SOCKET;

		// 关闭所有客户端连接
		for (auto& client : clients)
		{
			if (client->is_connected())
			{
				shutdown(client->get_socket(), SD_BOTH);
			}
		}

		// 等待线程结束
		if (trade_thread.joinable())
		{
			trade_thread.join();
		}

		for (auto& thread : client_threads)
		{
			if (thread.joinable())
			{
				thread.join();
			}
		}

		// 清空客户端列表
		clients.clear();
		client_threads.clear();

		std::cout << "Trading server stopped" << std::endl;
	}

private:
	void accept_clients()
	{
		while (running)
		{
			sockaddr_in client_addr;
			int addr_len = sizeof(client_addr);

			SOCKET client_socket = accept(server_socket, (sockaddr*)&client_addr, &addr_len);
			if (client_socket == INVALID_SOCKET)
			{
				if (running)
				{
					std::cerr << "Accept failed: " << WSAGetLastError() << std::endl;
				}
				continue;
			}

			std::cout << "Client connected: " << inet_ntoa(client_addr.sin_addr)
				<< ":" << ntohs(client_addr.sin_port) << std::endl;

			// 创建客户端连接
			auto client = std::make_unique<ClientConnection>(client_socket, next_client_id++, order_book);
			clients.push_back(std::move(client));

			// 启动客户端处理线程
			client_threads.emplace_back([this, idx = clients.size() - 1]()
			{
				clients[idx]->handle_client();
			});
		}
	}

	void trade_loop()
	{
		while (running)
		{
			// 执行交易
			auto trades = order_book.execute_trades();

			// 广播交易信息给所有客户端
			for (const auto& trade : trades)
			{
				broadcast_message(trade);
			}

			// 短暂休眠
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
	}

	void broadcast_message(const std::string& message)
	{
		for (auto& client : clients)
		{
			if (client->is_connected())
			{
				client->send_message(message);
			}
		}
	}
};

int main()
{
	try
	{
		TradingServer server;
		server.start(12345);

		// 等待用户输入停止服务器
		std::cout << "Press Enter to stop the server..." << std::endl;
		std::cin.get();

		server.stop();
	}
	catch (const std::exception& e)
	{
		std::cerr << "Error: " << e.what() << std::endl;
		return 1;
	}

	return 0;
}