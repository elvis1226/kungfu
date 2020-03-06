//
// Created by Keren Dong on 2020/1/14.
//

#ifndef KUNGFU_NODE_WATCHER_H
#define KUNGFU_NODE_WATCHER_H

#include <napi.h>

#include <kungfu/yijinjing/practice/apprentice.h>
#include "operators.h"
#include "journal.h"
#include "config_store.h"

namespace kungfu::node
{
    class Watcher : public Napi::ObjectWrap<Watcher>, public yijinjing::practice::apprentice
    {
    public:
        explicit Watcher(const Napi::CallbackInfo &info);

        ~Watcher();

        void NoSet(const Napi::CallbackInfo &info, const Napi::Value &value);

        Napi::Value GetLocator(const Napi::CallbackInfo &info);

        Napi::Value GetConfig(const Napi::CallbackInfo &info);

        Napi::Value GetState(const Napi::CallbackInfo &info);

        Napi::Value GetLedger(const Napi::CallbackInfo &info);

        Napi::Value IsUsable(const Napi::CallbackInfo &info);

        Napi::Value IsLive(const Napi::CallbackInfo &info);

        Napi::Value IsStarted(const Napi::CallbackInfo &info);

        Napi::Value Setup(const Napi::CallbackInfo &info);

        Napi::Value Step(const Napi::CallbackInfo &info);

        Napi::Value GetLocation(const Napi::CallbackInfo &info);

        Napi::Value PublishState(const Napi::CallbackInfo &info);

        Napi::Value isReadyToInteract(const Napi::CallbackInfo &info);

        Napi::Value IssueOrder(const Napi::CallbackInfo &info);

        Napi::Value CancelOrder(const Napi::CallbackInfo &info);

        static void Init(Napi::Env env, Napi::Object exports);

    protected:

        void react() override;

        void on_start() override;

    private:
        static Napi::FunctionReference constructor;
        yijinjing::data::location ledger_location_;
        Napi::ObjectReference config_ref_;
        Napi::ObjectReference state_ref_;
        Napi::ObjectReference ledger_ref_;
        serialize::JsUpdateState update_state;
        serialize::JsUpdateState update_ledger;
        serialize::JsPublishState publish;
        std::unordered_map<uint32_t, yijinjing::journal::writer_ptr> strategy_writers_;

        void RestoreState(const yijinjing::data::location_ptr &config_location);

        template<typename DataType>
        Napi::Value InteractWithTD(const Napi::CallbackInfo &info)
        {
            auto trigger_time = yijinjing::time::now_in_nano();
            Napi::Object obj = info[0].ToObject();
            DataType action = {};
            serialize::JsGet{}(obj, action);
            auto account_location = ExtractLocation(info, 1, get_locator());
            if (account_location and has_writer(account_location->uid))
            {
                auto writer = get_writer(account_location->uid);
                auto strategy_location = ExtractLocation(info, 2, get_locator());
                if (strategy_location and has_location(strategy_location->uid))
                {
                    auto proxy_location = yijinjing::data::location::make_shared(
                            strategy_location->mode, strategy_location->category,
                            get_io_device()->get_home()->group, strategy_location->name, strategy_location->locator
                    );
                    if (not has_channel(account_location->uid, proxy_location->uid))
                    {
                        longfist::types::RequestSimplexChannel request = {};
                        request.source_id = account_location->uid;
                        request.dest_id = proxy_location->uid;
                        writers_.at(get_master_commands_uid())->write(trigger_time, request);
                    }
                    writer->write_as(trigger_time, action, proxy_location->uid);
                } else
                {
                    writer->write(trigger_time, action);
                }
            }
            return Napi::Value();
        }
    };
}


#endif //KUNGFU_NODE_WATCHER_H
