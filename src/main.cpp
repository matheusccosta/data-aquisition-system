#include <cstdlib>
#include <iostream>
#include <memory>
#include <utility>
#include <boost/asio.hpp>
#include <fstream>
#include <vector>
#include <map>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <mutex>
#include <filesystem>
#include <algorithm>

namespace asio = boost::asio;
using asio::ip::tcp;
namespace fs = std::filesystem;

std::mutex file_mutex;
const std::string LOG_DIR = "logs";

#pragma pack(push, 1)
struct LogRecord {
    char sensor_id[32];
    std::time_t timestamp;
    double value;
};
#pragma pack(pop)

//função para converter string em time_t
std::time_t string_to_time_t(const std::string& time_string) {
    std::tm tm = {};
    std::istringstream ss(time_string);
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    return std::mktime(&tm);
}

//função para converter time_t em string
std::string time_t_to_string(std::time_t time_val) {
    std::tm* tm = std::localtime(&time_val);
    std::ostringstream ss;
    ss << std::put_time(tm, "%Y-%m-%dT%H:%M:%S");
    return ss.str();
}

class session : public std::enable_shared_from_this<session>
{
public:
  session(tcp::socket socket)
    : socket_(std::move(socket))
  {
  }

  void start()
  {
    read_message();
  }

private:
  void read_message()
  {
    auto self(shared_from_this());
    boost::asio::async_read_until(socket_, buffer_, "\r\n",
        [this, self](boost::system::error_code ec, std::size_t length)
        {
          if (!ec)
          {
            std::istream is(&buffer_);
            std::string message;
            std::getline(is, message);
            message.erase(message.find_last_not_of("\r\n") + 1);
            
            // std::cout << "testandoo: " << message << std::endl;  // debug
            
            ProcessMessage(message);
          }
        });
  }

  void ProcessMessage(const std::string& message) {
      std::vector<std::string> tokens;
      std::istringstream iss(message);
      std::string token;
      
      while (std::getline(iss, token, '|')) {
          tokens.push_back(token);
      }
      
      if (tokens.empty()) return;
      
      if (tokens[0] == "LOG" && tokens.size() == 4) {
          handle_log(tokens[1], tokens[2], tokens[3]);
      } else if (tokens[0] == "GET" && tokens.size() == 3) {
          handleGet(tokens[1], tokens[2]);
      } else {
          send_response("ERROR|INVALID_FORMAT\r\n");
      }
  }
  
  void handle_log(const std::string& sensor_id, const std::string& timestamp, const std::string& value_str) {
    try {
        double value = std::stod(value_str);
        std::time_t time_val = string_to_time_t(timestamp);
        
        LogRecord record;
        std::memset(record.sensor_id, 0, sizeof(record.sensor_id));
        std::strncpy(record.sensor_id, sensor_id.c_str(), sizeof(record.sensor_id) - 1);
        record.timestamp = time_val;
        record.value = value;
        
        std::lock_guard<std::mutex> lock(file_mutex);
        fs::create_directories(LOG_DIR);
        std::string filename = LOG_DIR + "/" + sensor_id + ".bin";
        std::ofstream file(filename, std::ios::binary | std::ios::app);
        if (file) {
            file.write(reinterpret_cast<const char*>(&record), sizeof(LogRecord));
        }
        // else { std::cout << "testaashu: erro ao abrir arquivo" << std::endl; }
    } catch (...) {
        send_response("ERROR|INVALID_LOG_DATA\r\n");
    }
    read_message();
  }
  
  void handleGet(const std::string& sensor_id, const std::string& num_records_str) {
    try {
        int num_records = std::stoi(num_records_str);
        if (num_records <= 0) {
            send_response("ERROR|INVALID_RECORD_COUNT\r\n");
            return;
        }
        
        std::lock_guard<std::mutex> lock(file_mutex);
        std::string filename = LOG_DIR + "/" + sensor_id + ".bin";
        std::ifstream file(filename, std::ios::binary | std::ios::ate);
        
        if (!file) {
            send_response("ERROR|INVALID_SENSOR_ID\r\n");
            return;
        }
        
        std::streamsize file_size = file.tellg();
        int total_records = file_size / sizeof(LogRecord);
        int records_to_read = std::min(num_records, total_records);
        
        if (records_to_read == 0) {
            send_response("0;\r\n");
            return;
        }
        
        std::vector<LogRecord> records(records_to_read);
        file.seekg(-records_to_read * sizeof(LogRecord), std::ios::end);
        file.read(reinterpret_cast<char*>(records.data()), records_to_read * sizeof(LogRecord));
        
        std::ostringstream response;
        response << records_to_read << ";";
        for (int i = 0; i < records_to_read; ++i) {
            response << time_t_to_string(records[i].timestamp) 
                     << "|" << records[i].value;
            if (i < records_to_read - 1) response << ";";
        }
        response << "\r\n";
        
        send_response(response.str());
    } catch (...) {
        send_response("ERROR|INVALID_REQUEST\r\n");
    }
  }
  
  void send_response(const std::string& response) {
    auto self(shared_from_this());
    // std::cout << "teasbfag: enviando " << response << std::endl;  // debug
    boost::asio::async_write(socket_, asio::buffer(response),
        [this, self](boost::system::error_code ec, std::size_t /*length*/)
        {
          if (!ec) {
            // read_message(); // antigo - causava loop
          }
        });
  }

  tcp::socket socket_;
  asio::streambuf buffer_;
};

class server
{
public:
  server(asio::io_context& io_context, short port)
    : acceptor_(io_context, tcp::endpoint(tcp::v4(), port))
  {
    accept();
  }

private:
  void accept()
  {
    acceptor_.async_accept(
        [this](boost::system::error_code ec, tcp::socket socket)
        {
          if (!ec)
          {
            std::make_shared<session>(std::move(socket))->start();
          }

          accept();
        });
  }

  tcp::acceptor acceptor_;
};

int main(int argc, char* argv[])
{
  if (argc != 2)
  {
    std::cerr << "Usage: server <port>\n";
    return 1;
  }

  asio::io_context io_context;

  server s(io_context, std::atoi(argv[1]));

  io_context.run();

  return 0;
}
