﻿#pragma once
#include "interface/handlers/EventHandler.hpp"
#include "netfilter/SocksRedirector.hpp"

class NetfilterPluginEventHandler : public QObject, public Qv2rayPlugin::handlers::event::PluginEventHandler
{
    Q_OBJECT
  public:
    NetfilterPluginEventHandler();
    void ProcessEvent(const Connectivity::EventObject &) override;

  private slots:
    void OnLog(size_t cid, QString protocol, bool isOpen, QString local, QString remote, int pid, QString path);

  private:
    NetFilterCore core;
};
