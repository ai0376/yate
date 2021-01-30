#include <yatephone.h>
#include <iostream>
#include <rediscli.h>
#include <boost/container/detail/singleton.hpp>

using namespace TelEngine;
namespace
{ // anonymous

    RedisCli *g_redisCli = &boost::container::dtl::singleton_default<RedisCli>::instance();
    class RedisModule : public Module
    {
    public:
        RedisModule();
        ~RedisModule();

    protected:
        virtual void initialize();
        virtual void statusModule(String &str);
        virtual void statusParams(String &str);
        virtual void statusDetail(String &str);
        virtual bool received(Message &msg, int id);

    private:
        bool m_init;
    };

    static RedisModule module;

    class RedisCliHandler : public MessageHandler
    {
    public:
        RedisCliHandler(unsigned int prio = 100)
            : MessageHandler("redis-database", prio, module.name())
        {
        }
        virtual bool received(Message &msg);
    };
    RedisModule::RedisModule() : Module("redisdb", "database", true),
                                 m_init(true)
    {
    }
    RedisModule::~RedisModule()
    {
    }
    void RedisModule::initialize()
    {
        Output("Initializing module RedisCli");
        Module::initialize();
        if (m_init)
        {
            Engine::install(new RedisCliHandler);
        }
        m_init = false;
    }
    void RedisModule::statusModule(String &str)
    {
    }
    void RedisModule::statusParams(String &str)
    {
    }
    void RedisModule::statusDetail(String &str)
    {
    }
    bool RedisModule::received(Message &msg, int id)
    {
        return true;
    }
    bool RedisCliHandler::received(Message &msg)
    {
        String query = msg.getValue("query");
        if (TelEngine::null(query))
        {
            return false;
        }
        ObjList *l = query.split(';');
        String cmd;
        std::deque<redisclient::RedisBuffer> args;
        bool flag = true;
        for (ObjList *o = l->skipNull(); o; o = o->skipNext())
        {
            if (flag)
            {
                cmd = static_cast<String *>(o->get());
                flag = false;
            }
            else
            {
                String arg = static_cast<String *>(o->get());
                args.push_back(arg.safe());
            }
        }
        TelEngine::destruct(l);
        if (msg.getBoolValue("results", true))
        {
            std::promise<bool> result;
            g_redisCli->ExecCommand(cmd.safe(), args, [&result](redisclient::RedisValue value) {
                if (value.isOk())
                {
                    result.set_value(true);
                }
                else
                {
                    result.set_value(false);
                }
            });
            result.get_future().get();
        }
        else
        {
            g_redisCli->ExecCommand(cmd.safe(), args, [](redisclient::RedisValue value) {
                if (value.isOk())
                {
                    Output("value is ok\n");
                }
                else
                {
                    Output("value is error\n");
                }
            });
        }
    }

}; // namespace