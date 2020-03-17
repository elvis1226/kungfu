//
// Created by qlu on 2019/1/14.
//

#include <chrono>
#include <kungfu/wingchun/encoding.h>
#include "trader_ctp.h"
#include "serialize_ctp.h"
#include "type_convert_ctp.h"

using namespace kungfu::longfist;
using namespace kungfu::longfist::types;
using namespace kungfu::yijinjing;

namespace kungfu::wingchun::ctp
{
    TraderCTP::TraderCTP(bool low_latency, yijinjing::data::locator_ptr locator, const std::string &account_id,
                         const std::string &json_config) :
            Trader(low_latency, std::move(locator), SOURCE_CTP, account_id), front_id_(-1), session_id_(-1),
            order_ref_(-1), request_id_(0), api_(nullptr)
    {
        yijinjing::log::copy_log_settings(get_io_device()->get_home(), SOURCE_CTP);
        config_ = nlohmann::json::parse(json_config);
    }

    void TraderCTP::on_start()
    {
        broker::Trader::on_start();

        if (api_ == nullptr)
        {
            std::string runtime_folder = get_runtime_folder();
            SPDLOG_INFO("create ctp td api with path: {}", runtime_folder);
            api_ = CThostFtdcTraderApi::CreateFtdcTraderApi(runtime_folder.c_str());
            api_->RegisterSpi(this);
            api_->RegisterFront((char *) config_.td_uri.c_str());
            api_->SubscribePublicTopic(THOST_TERT_QUICK);
            api_->SubscribePrivateTopic(THOST_TERT_QUICK);
            api_->Init();
        }
    }

    bool TraderCTP::login()
    {
        CThostFtdcReqUserLoginField login_field = {};
        strcpy(login_field.TradingDay, "");
        strcpy(login_field.UserID, config_.account_id.c_str());
        strcpy(login_field.BrokerID, config_.broker_id.c_str());
        strcpy(login_field.Password, config_.password.c_str());
        int rtn = api_->ReqUserLogin(&login_field, ++request_id_);
        if (rtn != 0)
        {
            SPDLOG_ERROR("failed to request login for UserID {} BrokerID {}, error id: {}", login_field.UserID,
                         login_field.BrokerID, rtn);
        }
        return rtn == 0;
    }

    bool TraderCTP::req_settlement_confirm()
    {
        CThostFtdcSettlementInfoConfirmField req = {};
        strcpy(req.InvestorID, config_.account_id.c_str());
        strcpy(req.BrokerID, config_.broker_id.c_str());
        int rtn = api_->ReqSettlementInfoConfirm(&req, ++request_id_);
        return rtn == 0;
    }

    bool TraderCTP::req_auth()
    {
        struct CThostFtdcReqAuthenticateField req = {};
        strcpy(req.UserID, config_.account_id.c_str());
        strcpy(req.BrokerID, config_.broker_id.c_str());
        if (config_.product_info.length() > 0)
        {
            strcpy(req.UserProductInfo, config_.product_info.c_str());
        }
        strcpy(req.AppID, config_.app_id.c_str());
        strcpy(req.AuthCode, config_.auth_code.c_str());
        int rtn = this->api_->ReqAuthenticate(&req, ++request_id_);
        if (rtn != 0)
        {
            SPDLOG_ERROR("failed to req auth, error id = {}", rtn);
        }
        return rtn == 0;
    }

    bool TraderCTP::req_account()
    {
        CThostFtdcQryTradingAccountField req = {};
        strcpy(req.BrokerID, config_.broker_id.c_str());
        strcpy(req.InvestorID, config_.account_id.c_str());
        int rtn = api_->ReqQryTradingAccount(&req, ++request_id_);
        return rtn == 0;
    }

    bool TraderCTP::req_position()
    {
        long_position_map_.clear();
        short_position_map_.clear();
        CThostFtdcQryInvestorPositionField req = {};
        strcpy(req.BrokerID, config_.broker_id.c_str());
        strcpy(req.InvestorID, config_.account_id.c_str());
        int rtn = api_->ReqQryInvestorPosition(&req, ++request_id_);
        return rtn == 0;
    }

    bool TraderCTP::req_position_detail()
    {
        CThostFtdcQryInvestorPositionDetailField req = {};
        strcpy(req.BrokerID, config_.broker_id.c_str());
        strcpy(req.InvestorID, config_.account_id.c_str());
        int rtn = api_->ReqQryInvestorPositionDetail(&req, ++request_id_);
        return rtn == 0;
    }

    bool TraderCTP::req_qry_instrument()
    {
        CThostFtdcQryInstrumentField req = {};
        int rtn = api_->ReqQryInstrument(&req, ++request_id_);
        return rtn == 0;
    }

    bool TraderCTP::insert_order(const event_ptr &event)
    {
        const OrderInput &input = event->data<OrderInput>();

        CThostFtdcInputOrderField ctp_input;
        memset(&ctp_input, 0, sizeof(ctp_input));

        to_ctp(ctp_input, input);
        strcpy(ctp_input.BrokerID, config_.broker_id.c_str());
        strcpy(ctp_input.InvestorID, config_.account_id.c_str());

        order_ref_++;
        strcpy(ctp_input.OrderRef, std::to_string(order_ref_).c_str());

        int error_id = api_->ReqOrderInsert(&ctp_input, ++request_id_);
        SPDLOG_TRACE(to_string(ctp_input));

        auto nano = time::now_in_nano();
        auto writer = get_writer(event->source());
        Order &order = writer->open_data<Order>(event->gen_time());
        order_from_input(input, order);
        order.insert_time = nano;
        order.update_time = nano;

        if (error_id == 0)
        {
            outbound_orders_[input.order_id] = order_ref_;
            inbound_order_refs_[ctp_input.OrderRef] = input.order_id;
            SPDLOG_INFO("FrontID: {} SessionID: {} OrderRef: {}", front_id_, session_id_, ctp_input.OrderRef);
        } else
        {
            order.error_id = error_id;
            order.status = OrderStatus::Error;
            SPDLOG_ERROR("failed to insert order {}, (error_id) {}", input.order_id, error_id);
        }

        orders_.emplace(order.uid(), state<Order>(event->dest(), event->source(), nano, order));
        writer->close_data();
        return error_id == 0;
    }

    bool TraderCTP::cancel_order(const event_ptr &event)
    {
        const OrderAction &action = event->data<OrderAction>();
        if (outbound_orders_.find(action.order_id) == outbound_orders_.end())
        {
            SPDLOG_ERROR("failed to cancel order {}, can't find related ctp order id", action.order_id);
            return false;
        }
        auto ctp_order_ref = outbound_orders_[action.order_id];
        auto order_state = orders_.at(action.order_id);

        CThostFtdcInputOrderActionField ctp_action = {};
        strcpy(ctp_action.BrokerID, config_.broker_id.c_str());
        strcpy(ctp_action.InvestorID, config_.account_id.c_str());
        strcpy(ctp_action.OrderRef, std::to_string(ctp_order_ref).c_str());
        ctp_action.FrontID = front_id_;
        ctp_action.SessionID = session_id_;
        ctp_action.ActionFlag = THOST_FTDC_AF_Delete;
        strcpy(ctp_action.InstrumentID, order_state.data.instrument_id);
        strcpy(ctp_action.ExchangeID, order_state.data.exchange_id);

        int error_id = api_->ReqOrderAction(&ctp_action, ++request_id_);
        if (error_id == 0)
        {
            inbound_actions_.emplace(request_id_, action.uid());
            actions_.emplace(action.uid(), state<OrderAction>(event->dest(), event->source(), event->gen_time(), action));
            SPDLOG_TRACE("requested cancel order {}, order action id {}, request id {}", action.order_id,
                         action.order_action_id, request_id_);
        } else
        {
            auto writer = get_writer(event->source());
            OrderActionError &error = writer->open_data<OrderActionError>(event->gen_time());
            error.error_id = error_id;
            error.order_id = action.order_id;
            error.order_action_id = action.order_action_id;
            writer->close_data();
            SPDLOG_ERROR("failed to cancel order {}, error_id: {}", action.order_id, error_id);
        }
        return error_id == 0;
    }

    void TraderCTP::OnFrontConnected()
    {
        SPDLOG_INFO("connected");
        req_auth();
    }

    void TraderCTP::OnFrontDisconnected(int nReason)
    {
        SPDLOG_ERROR("disconnected [{}] {}", nReason, disconnected_reason(nReason));
    }

    void TraderCTP::OnRspAuthenticate(CThostFtdcRspAuthenticateField *pRspAuthenticateField,
                                      CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
    {
        if (pRspInfo != nullptr && pRspInfo->ErrorID != 0)
        {
            SPDLOG_ERROR("failed to authenticate, ErrorId: {} ErrorMsg: {}", pRspInfo->ErrorID,
                         gbk2utf8(pRspInfo->ErrorMsg));
            return;
        }
        login();
    }

    void TraderCTP::OnRspUserLogin(CThostFtdcRspUserLoginField *pRspUserLogin, CThostFtdcRspInfoField *pRspInfo,
                                   int nRequestID, bool bIsLast)
    {
        if (pRspInfo != nullptr && pRspInfo->ErrorID != 0)
        {
            SPDLOG_ERROR("ErrorId) {} (ErrorMsg) {}", pRspInfo->ErrorID, gbk2utf8(pRspInfo->ErrorMsg));
            return;
        }
        SPDLOG_INFO("login success, BrokerID: {} UserID: {} SystemName: {} TradingDay: {} FrontID: {} SessionID: {}",
                    pRspUserLogin->BrokerID, pRspUserLogin->UserID, pRspUserLogin->SystemName,
                    pRspUserLogin->TradingDay, pRspUserLogin->FrontID, pRspUserLogin->SessionID);
        session_id_ = pRspUserLogin->SessionID;
        front_id_ = pRspUserLogin->FrontID;
        order_ref_ = atoi(pRspUserLogin->MaxOrderRef);
        trading_day_ = pRspUserLogin->TradingDay;
        req_settlement_confirm();
    }

    void TraderCTP::OnRspUserLogout(CThostFtdcUserLogoutField *pUserLogout, CThostFtdcRspInfoField *pRspInfo,
                                    int nRequestID, bool bIsLast)
    {}

    void TraderCTP::OnRspSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField *pSettlementInfoConfirm,
                                               CThostFtdcRspInfoField *pRspInfo,
                                               int nRequestID, bool bIsLast)
    {
        if (pRspInfo != nullptr && pRspInfo->ErrorID != 0)
        {
            SPDLOG_ERROR("failed confirm settlement info, ErrorId: {} ErrorMsg: {}", pRspInfo->ErrorID,
                         gbk2utf8(pRspInfo->ErrorMsg));
        }
        update_broker_state(BrokerState::Ready);
        std::this_thread::sleep_for(std::chrono::seconds(1));
        req_qry_instrument();
    }

    void TraderCTP::OnRspOrderInsert(CThostFtdcInputOrderField *pInputOrder, CThostFtdcRspInfoField *pRspInfo,
                                     int nRequestID, bool bIsLast)
    {
        if (pRspInfo != nullptr && pRspInfo->ErrorID != 0)
        {
            auto order_id = inbound_order_refs_[pInputOrder->OrderRef];
            if (orders_.find(order_id) != orders_.end())
            {
                auto order_state = orders_.at(order_id);
                order_state.data.status = OrderStatus::Error;
                order_state.data.error_id = pRspInfo->ErrorID;
                strncpy(order_state.data.error_msg, gbk2utf8(pRspInfo->ErrorMsg).c_str(), ERROR_MSG_LEN);
                write_to(0, order_state.data, order_state.dest);
            }
            SPDLOG_ERROR("failed to insert order, ErrorId: {} ErrorMsg: {}, InputOrder: {}",
                         pRspInfo->ErrorID, gbk2utf8(pRspInfo->ErrorMsg),
                         pInputOrder == nullptr ? "" : to_string(*pInputOrder));
        }
    }

    void TraderCTP::OnRspOrderAction(CThostFtdcInputOrderActionField *pInputOrderAction,
                                     CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
    {
        if (pRspInfo != nullptr && pRspInfo->ErrorID != 0)
        {
            auto action_id = inbound_actions_.at(nRequestID);
            if (actions_.find(action_id) != actions_.end())
            {
                auto &action_state = actions_.at(action_id);
                auto &action = action_state.data;
                uint32_t source = (action.order_action_id >> 32) xor get_home_uid();
                if (has_writer(source))
                {
                    auto writer = get_writer(source);
                    OrderActionError &error = writer->open_data<OrderActionError>(0);
                    error.error_id = pRspInfo->ErrorID;
                    strncpy(error.error_msg, gbk2utf8(pRspInfo->ErrorMsg).c_str(), ERROR_MSG_LEN);
                    error.order_id = action.order_id;
                    error.order_action_id = action.order_action_id;
                    writer->close_data();
                } else
                {
                    SPDLOG_ERROR("has no writer for [{:08x}]", source);
                }
            }
            SPDLOG_ERROR("ErrorId) {} (ErrorMsg) {} (InputOrderAction) {} (nRequestID) {} (bIsLast) {}",
                         pRspInfo->ErrorID, gbk2utf8(pRspInfo->ErrorMsg),
                         pInputOrderAction == nullptr ? "" : to_string(*pInputOrderAction), nRequestID, bIsLast);
        }
    }

    void TraderCTP::OnRtnOrder(CThostFtdcOrderField *pOrder)
    {
        SPDLOG_TRACE(to_string(*pOrder));
        auto order_id = inbound_order_refs_[pOrder->OrderRef];
        inbound_order_sysids_[pOrder->OrderSysID] = order_id;
        if (orders_.find(order_id) == orders_.end())
        {
            SPDLOG_ERROR("can't find FrontID {} SessionID {} OrderRef {}", pOrder->FrontID, pOrder->SessionID,
                         pOrder->OrderRef);
            return;
        }
        auto order_state = orders_.at(order_id);
        auto writer = get_writer(order_state.dest);
        Order &order = writer->open_data<Order>(0);
        from_ctp(*pOrder, order);
        order.order_id = order_state.data.order_id;
        order.parent_id = order_state.data.parent_id;
        order.insert_time = order_state.data.insert_time;
        order.update_time = time::now_in_nano();
        writer->close_data();
    }

    void TraderCTP::OnRtnTrade(CThostFtdcTradeField *pTrade)
    {
        SPDLOG_TRACE(to_string(*pTrade));
        auto order_id = inbound_order_sysids_[pTrade->OrderSysID];
        if (orders_.find(order_id) == orders_.end())
        {
            SPDLOG_ERROR("can't find order with OrderSysID: {}", pTrade->OrderSysID);
            return;
        }
        auto order_state = orders_.at(order_id);
        auto writer = get_writer(order_state.dest);
        Trade &trade = writer->open_data<Trade>(0);
        from_ctp(*pTrade, trade);
        strncpy(trade.trading_day, trading_day_.c_str(), DATE_LEN);
        uint64_t trade_id = writer->current_frame_uid();
        trade.trade_id = trade_id;
        trade.order_id = order_state.data.order_id;
        trade.parent_order_id = order_state.data.parent_id;
        inbound_trade_ids_[pTrade->TradeID] = trade.uid();
        trades_.emplace(trade.uid(), state<Trade>(order_state.source, order_state.dest, time::now_in_nano(), trade));
        writer->close_data();
    }

    void TraderCTP::OnRspQryTradingAccount(CThostFtdcTradingAccountField *pTradingAccount,
                                           CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
    {
        if (pRspInfo != nullptr && pRspInfo->ErrorID != 0)
        {
            SPDLOG_ERROR("(error_id) {} (error_msg) {}", pRspInfo->ErrorID, gbk2utf8(pRspInfo->ErrorMsg));
            return;
        }
        SPDLOG_TRACE(to_string(*pTradingAccount));
        auto writer = get_writer(0);
        Asset &account = writer->open_data<Asset>(0);
        strcpy(account.account_id, get_account_id().c_str());
        from_ctp(*pTradingAccount, account);
        account.update_time = time::now_in_nano();
        account.holder_uid = get_io_device()->get_home()->uid;
        writer->close_data();
        std::this_thread::sleep_for(std::chrono::seconds(1));
        req_position();
    }

    void TraderCTP::OnRspQryInvestorPosition(CThostFtdcInvestorPositionField *pInvestorPosition,
                                             CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
    {
        if (pRspInfo != nullptr && pRspInfo->ErrorID != 0)
        {
            SPDLOG_ERROR("(error_id) {} (error_msg) {}", pRspInfo->ErrorID, gbk2utf8(pRspInfo->ErrorMsg));
        } else if (pInvestorPosition != nullptr)
        {
            SPDLOG_TRACE(to_string(*pInvestorPosition));
            auto &position_map =
                    pInvestorPosition->PosiDirection == THOST_FTDC_PD_Long ? long_position_map_ : short_position_map_;
            if (position_map.find(pInvestorPosition->InstrumentID) == position_map.end())
            {
                Position position = {};
                strncpy(position.trading_day, pInvestorPosition->TradingDay, DATE_LEN);
                strncpy(position.instrument_id, pInvestorPosition->InstrumentID, INSTRUMENT_ID_LEN);
                strncpy(position.exchange_id, pInvestorPosition->ExchangeID, EXCHANGE_ID_LEN);
                strncpy(position.account_id, pInvestorPosition->InvestorID, ACCOUNT_ID_LEN);
                position.holder_uid = get_io_device()->get_home()->uid;
                position.direction =
                        pInvestorPosition->PosiDirection == THOST_FTDC_PD_Long ? Direction::Long : Direction::Short;
                position_map[pInvestorPosition->InstrumentID] = position;
            }
            auto &position = position_map[pInvestorPosition->InstrumentID];
            auto &inst_info = instrument_map_[pInvestorPosition->InstrumentID];
            if (strcmp(pInvestorPosition->ExchangeID, EXCHANGE_SHFE) == 0)
            {
                if (pInvestorPosition->YdPosition > 0 && pInvestorPosition->TodayPosition <= 0)
                {
                    position.yesterday_volume = pInvestorPosition->Position;
                }
            } else
            {
                position.yesterday_volume = pInvestorPosition->Position - pInvestorPosition->TodayPosition;
            }
            position.volume += pInvestorPosition->Position;
            position.margin += pInvestorPosition->ExchangeMargin;
            if (position.volume > 0)
            {
                double cost = position.avg_open_price * (position.volume - pInvestorPosition->Position) *
                              inst_info.contract_multiplier + pInvestorPosition->OpenCost;
                position.avg_open_price = cost / (position.volume * inst_info.contract_multiplier);
            }
        }
        if (bIsLast)
        {
            SPDLOG_TRACE("RequestID {}", nRequestID);
            auto writer = get_writer(0);
            for (const auto &kv: long_position_map_)
            {
                const auto &position = kv.second;
                writer->write(0, Position::tag, position);
            }
            for (const auto &kv: short_position_map_)
            {
                const auto &position = kv.second;
                writer->write(0, Position::tag, position);
            }
            PositionEnd &end = writer->open_data<PositionEnd>(0);
            end.holder_uid = get_io_device()->get_home()->uid;
            writer->close_data();
            short_position_map_.clear();
            long_position_map_.clear();
        }
    }

    void TraderCTP::OnRspQryInvestorPositionDetail(CThostFtdcInvestorPositionDetailField *pInvestorPositionDetail,
                                                   CThostFtdcRspInfoField *pRspInfo, int nRequestID,
                                                   bool bIsLast)
    {
        if (pRspInfo != nullptr && pRspInfo->ErrorID != 0)
        {
            SPDLOG_ERROR("(error_id) {} (error_msg) {}", pRspInfo->ErrorID, gbk2utf8(pRspInfo->ErrorMsg));
        } else
        {
            auto writer = get_writer(0);
            if (pInvestorPositionDetail != nullptr)
            {
                SPDLOG_TRACE(to_string(*pInvestorPositionDetail));
                PositionDetail &pos_detail = writer->open_data<PositionDetail>(0);
                from_ctp(*pInvestorPositionDetail, pos_detail);
                pos_detail.update_time = time::now_in_nano();
                if (inbound_trade_ids_.find(pInvestorPositionDetail->TradeID) != inbound_trade_ids_.end())
                {
                    auto trade_id = inbound_trade_ids_.at(pInvestorPositionDetail->TradeID);
                    auto &trade = trades_.at(trade_id).data;
                    pos_detail.trade_id = trade.trade_id;
                    pos_detail.trade_time = trade.trade_time;
                } else
                {
                    pos_detail.trade_id = writer->current_frame_uid();
                }
                writer->close_data();
            }
        }
        if (bIsLast)
        {
            SPDLOG_TRACE("RequestID {}", nRequestID);
            get_writer(0)->mark(0, PositionDetailEnd::tag);
        }
    }

    void TraderCTP::OnRspQryInstrument(CThostFtdcInstrumentField *pInstrument, CThostFtdcRspInfoField *pRspInfo,
                                       int nRequestID, bool bIsLast)
    {
        if (pRspInfo != nullptr && pRspInfo->ErrorID != 0)
        {
            SPDLOG_ERROR("(error_id) {} (error_msg) {}", pRspInfo->ErrorID, gbk2utf8(pRspInfo->ErrorMsg));
            return;
        }
        SPDLOG_TRACE(kungfu::wingchun::ctp::to_string(*pInstrument));
        auto writer = get_writer(0);
        if (pInstrument->ProductClass == THOST_FTDC_PC_Futures)
        {
            Instrument &instrument = writer->open_data<Instrument>(0);
            from_ctp(*pInstrument, instrument);
            instrument_map_[pInstrument->InstrumentID] = instrument;
            writer->close_data();
        }
        if (bIsLast)
        {
            writer->mark(0, InstrumentEnd::tag);
            std::this_thread::sleep_for(std::chrono::seconds(1));
            req_account();
        }
    }
}

