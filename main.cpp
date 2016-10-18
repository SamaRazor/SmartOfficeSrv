#include <iostream>
#include "smartoffice-srv.h"
#include <stdlib.h>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <utility>
#include <boost/asio.hpp>
#include <string>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/regex.hpp>
#include <mysql++.h>
#include <boost/array.hpp>
#include <boost/thread.hpp>
#include <vector>
#include <atomic>

using boost::asio::ip::tcp;
using namespace std;
using namespace boost;

class mysql_handler
{
private:
    mysqlpp::Connection *conn;
    vector<string> *hashes;
public:
    bool connected = false;

    mysql_handler() {
        hashes = new vector<string>;
        conn = new mysqlpp::Connection(true);
    }

    void connect(string database, string address, string user, string password) {
        conn->set_option(new mysqlpp :: SetCharsetNameOption ("utf8"));
        try {
            conn->connect(database.c_str(), address.c_str(), user.c_str(), password.c_str());
            connected = true;
        }
        catch (mysqlpp::Exception e) {
            cerr << e.what();
        }
    }

    mysql_handler(string database, string address, string user, string password) {
        hashes = new vector<string>;
        conn = new mysqlpp::Connection(true);
        connect(database, address, user, password);
    }

    ~mysql_handler() {
        delete conn;
        delete hashes;
    }

    void refresh_hashes() {
        if (connected) {
            try {
                mysqlpp::Query query = conn->query("SELECT hash from nodes");
                mysqlpp::StoreQueryResult res = query.store();
                if (res) {
                    mysqlpp::StoreQueryResult::const_iterator it;
                    for (it = res.begin(); it != res.end(); ++it) {
                        mysqlpp::Row row = *it;
                        hashes->push_back(row[0].c_str());
                    }
                }
            }
            catch (mysqlpp::Exception e) {
                cerr << e.what();
            }
        }
    }

    void print_hashes() {
        for (auto it = hashes->begin(); it != hashes->end(); ++it) {
            cout << *it << endl;
        }
    }

    bool is_user_exists(string hash) {
        return std::find(hashes->begin(), hashes->end(), hash) != hashes->end();
    }

};

mysql_handler *mysql = new mysql_handler();

class session;
std::vector<session*> sessions;

enum {
    OK = 200,
    FORBIDDEN = 403,
    UNAUTHORIZED = 401,
    INTERNAL_SERVER_ERROR = 500
};


class session
        : public std::enable_shared_from_this<session>
{
private:
    std::string  *node_id = new string("");
public:
    string ip;
    bool active = false;


    std::string get_node_id() {
        if (*node_id != "") {
            return *node_id;
        } else return "Unathorized";
    }

    session(tcp::socket socket) : socket_(std::move(socket))
    {
        active = false;
        memset(transmitted_data_, 0, sizeof(transmitted_data_));
        memset(recieved_data_, 0, sizeof(recieved_data_));
        *node_id = "";
    }

    ~session()
    {
        cout << "SESSION DC (node_id: " << this->get_node_id() << ")" << std::endl;
        for (uint32_t i = 0; i < sessions.size(); i++) {
            if (this == sessions[i]) {
                sessions.erase(sessions.begin() + i);
                break;
            }
        }
        delete node_id;
    }

    void start()
    {
        do_read();
    }

private:

    enum {
        max_length = 1024
    };

    char request_header[max_length];
    char request_body[max_length];

    void handle_request(size_t length) {
        active = true;
        vector<string> strs;
        vector<string> buf;

        string headers_s;

        map<string,string> headers;

        typedef map<string, int, less<string> > map_type;
        regex expression("^(.+(?:\r\n|\n|\r))+(?:\r\n|\n|\r)(.*)$");

        string::const_iterator start, end;
        string data(recieved_data_);

        start = data.begin();
        end = data.begin() + length;

        match_results<string::const_iterator> what;
        match_flag_type flags = regex_constants::match_single_line | regex_constants::match_stop;

        while (regex_search(start, end, what, expression, flags)) {
            start = what[0].second;
            string header(what[1].first, what[1].second);
            headers_s = header;
            string body(what[2].first, what[2].second);
        }

        strs.clear();
        split(strs,headers_s,is_any_of("\n"));

        strs.erase(strs.end() - 1);

        for(vector<string>::iterator it = strs.begin(); it != strs.end(); ++it) {
            buf.clear();
            split(buf,*it,is_any_of(":"));
            headers[buf[0]] = buf[1].substr(1,buf[1].size());
        }

        // Если запрос не содержит идентификатор.
        if (headers.find("node_id") == headers.end()) {
            strcpy(transmitted_data_, "status: 400\n");
        } else {
            if (headers.find("node_id") != headers.end()) {
                *node_id = (*headers.find("node_id")).second;
            } else
                *node_id = "";

            mysql->refresh_hashes();
            if (mysql->is_user_exists(*node_id)) {
                if (headers["action"] == "call")
                    // Звонок.
                    cout << "RING" << endl;
                strcpy(transmitted_data_, "status: 200\n");
            } else
                strcpy(transmitted_data_, "status: 401\n");

        }
    }

    void do_read()
    {
        auto self(shared_from_this());
        socket_.async_read_some(
            asio::buffer(recieved_data_, max_length),
            [this, self](system::error_code ec, size_t length) {
                if (!ec) {
                    cout << "Recieved from: " << socket_.remote_endpoint().address().to_string() << endl;
                    cout << recieved_data_ << endl;
                    handle_request(length);
                    do_write(length);
                }
            }
        );
    }

    void do_write(size_t length)
    {
        auto self(shared_from_this());
        asio::async_write(
            socket_, asio::buffer(transmitted_data_, std::strlen(transmitted_data_)),
            [this, self](system::error_code ec, size_t length) {
                 if (!ec) {
                     do_read();
                 }
            }
        );
    }
    tcp::socket socket_;
    char transmitted_data_[max_length];
    char recieved_data_[max_length];

public:

    void send_message(std::string message) {
        strcpy(transmitted_data_, message.c_str());
        do_write(message.length());
    }
};

class server
{
public:

    server(asio::io_service& io_service, short port)
            : acceptor_(io_service, tcp::endpoint(tcp::v4(), port)),
              socket_(io_service) {
        do_accept();
    }

    void send_message(std::string node_id, std::string message) {
        for (auto it = sessions.begin(); it != sessions.end(); ++it) {
            if ((*it)->get_node_id() == node_id) {
                (*it)->send_message(message);
            }
        }
    }

    void send_message(std::string host, int port, std::string message) {
        boost::system::error_code ec;
        boost::asio::io_service ios;

        boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::address::from_string(host), port);

        boost::asio::ip::tcp::socket socket(ios);

        socket.connect(endpoint);

        boost::array<char, 2048> buf;
        std::copy(message.begin(),message.end(),buf.begin());
        boost::system::error_code error;
        socket.write_some(boost::asio::buffer(buf, message.size()), error);

        std::string response;

        do {
            char buf[1024];
            size_t bytes_transferred = socket.receive(asio::buffer(buf, 1024), {}, ec);
            if (!ec) response.append(buf, buf + bytes_transferred);
        } while (!ec);
        socket.close();
    }

private:
    char error_data[1000];
    std::size_t error_size;
    void do_accept() {
        acceptor_.async_accept(
            socket_,
            [this](system::error_code ec) {
                std::shared_ptr<session> *sp = nullptr;

                if (!ec) {
                    sp = new std::shared_ptr<session>;
                    *sp = std::make_shared<session>(std::move(socket_));
                    sessions.push_back(sp->get());
                    (*sp)->start();
                } else {
                    cout << ec << std::endl;
                }

                do_accept();
                delete sp;
                cout << "END\n\r";
            }
        );

    }
    tcp::acceptor acceptor_;
    tcp::socket socket_;
};
asio::io_service io_service;
server s(io_service, 2525); //Починить это говно!

void start_server(short port) {
    io_service.run();
}

int main(int argc, char* argv[])
{
    short port;
    std::string database, server_address, user, password;
    property_tree::ptree pt;
    property_tree::ini_parser::read_ini("config.ini", pt);
    istringstream (pt.get<string>("General.port")) >> port;
    istringstream (pt.get<string>("MySQL.database")) >> database;
    istringstream (pt.get<string>("MySQL.address")) >> server_address;
    istringstream (pt.get<string>("MySQL.user")) >> user;
    istringstream (pt.get<string>("MySQL.password")) >> password;
    mysql->connect(database, server_address, user, password);
    mysql->refresh_hashes();
    mysql->print_hashes();
    try {
        boost::thread{start_server, port};

        char chars[20];
        while (true) {
            scanf("%s",chars);
            if(chars[0] == 'a')
            s.send_message("biba", "ZDAROVA");
            if(chars[0] == 'b')
            cout << sessions.size();
        }
    }
    catch (std::exception &e) {
        cerr << "Exception: " << e.what() << "\n";
    }

    delete mysql;
    return 0;
}