#ifndef _REDIS_CLI
#define _REDIS_CLI

#include <redisclient/redisasyncclient.h>
#include <boost/thread.hpp>
#include <boost/asio.hpp>

/* build redisclient header

cd ${redisclient_root}
mkdir build
cd build
cmake -DCMAKE_INSTALL_PREFIX=../install -DHEADER_ONLY=yes ..
make install

*/
class RedisCli : boost::noncopyable
{
public:
    RedisCli(std::string ip = "127.0.0.1", unsigned short port = 6379, std::string password = "")
        : m_io{},
          m_work(new boost::asio::io_service::work(m_io)),
          m_thread(new boost::thread(boost::bind(&boost::asio::io_service::run, &m_io))),
          m_cli(new redisclient::RedisAsyncClient(m_io))
    {
        m_cli->installErrorHandler(std::bind(&RedisCli::errorHandler, this, std::placeholders::_1));
        boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::address::from_string(ip), port);
        m_cli->connect(endpoint, [this, password](boost::system::error_code err) {
            if (!err) //connect error
            {
                if (!password.empty())
                {
                    m_cli->command("auth", {password}, [](redisclient::RedisValue value) {
                        if (value.isOk())
                        {
                            printf("redis auth success\n");
                        }
                        else
                        {
                            printf("redis auth failed\n");
                        }
                    });
                }
                printf("redis connect success\n");
            }
            else
            {
                printf("redis connect error %d\n", err);
            }
        });
    }
    ~RedisCli()
    {
        m_cli.reset();
        m_work.reset();
        m_thread->join();
    }
    void ExecCommand(const std::string &cmd, std::deque<redisclient::RedisBuffer> args, std::function<void(redisclient::RedisValue)> handler)
    {
        if (m_cli)
        {
            m_cli->command(cmd, args, handler);
        }
    }

private:
    void errorHandler(const std::string &err)
    {
        printf("RedisCli::errorHandler %s\n", err.c_str());
    }

private:
    boost::asio::io_service m_io;
    std::shared_ptr<boost::asio::io_service::work> m_work;
    std::shared_ptr<boost::thread> m_thread;

    std::shared_ptr<redisclient::RedisAsyncClient> m_cli;
};

#endif