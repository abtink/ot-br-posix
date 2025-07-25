/*
 *    Copyright (c) 2021, The OpenThread Authors.
 *    All rights reserved.
 *
 *    Redistribution and use in source and binary forms, with or without
 *    modification, are permitted provided that the following conditions are met:
 *    1. Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *    2. Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *    3. Neither the name of the copyright holder nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 *    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *    AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *    IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *    ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *    LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *    CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *    SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *    INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *    CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *    ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *    POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file
 *   The file implements the OTBR Agent.
 */

#define OTBR_LOG_TAG "APP"

#ifdef HAVE_LIBSYSTEMD
#include <systemd/sd-daemon.h>
#endif

#include "agent/application.hpp"
#include "common/code_utils.hpp"
#include "common/mainloop_manager.hpp"
#include "host/posix/dnssd.hpp"
#include "utils/infra_link_selector.hpp"

namespace otbr {

#ifndef OTBR_MAINLOOP_POLL_TIMEOUT_SEC
#define OTBR_MAINLOOP_POLL_TIMEOUT_SEC 10
#endif

std::atomic_bool     Application::sShouldTerminate(false);
const struct timeval Application::kPollTimeout = {OTBR_MAINLOOP_POLL_TIMEOUT_SEC, 0};

Application::Application(Host::ThreadHost  &aHost,
                         const std::string &aInterfaceName,
                         const std::string &aBackboneInterfaceName)
    : mInterfaceName(aInterfaceName)
    , mBackboneInterfaceName(aBackboneInterfaceName)
    , mHost(aHost)
#if OTBR_ENABLE_MDNS
    , mPublisher(
          Mdns::Publisher::Create([this](Mdns::Publisher::State aState) { mMdnsStateSubject.UpdateState(aState); }))
#endif
#if OTBR_ENABLE_DNSSD_PLAT
    , mDnssdPlatform(*mPublisher)
#endif
#if OTBR_ENABLE_BORDER_AGENT
    , mBorderAgent(*mPublisher)
    , mBorderAgentUdpProxy(mHost)
#endif
#if OTBR_ENABLE_DBUS_SERVER
    , mDBusAgent(MakeDBusDependentComponents())
#endif
{
    if (mHost.GetCoprocessorType() == OT_COPROCESSOR_RCP)
    {
        CreateRcpMode();
    }
    else if (mHost.GetCoprocessorType() == OT_COPROCESSOR_NCP)
    {
        CreateNcpMode();
    }
    else
    {
        DieNow("Unknown Co-processor type!");
    }
}

void Application::Init(const std::string &aRestListenAddress, int aRestListenPort)
{
    mHost.Init();

    switch (mHost.GetCoprocessorType())
    {
    case OT_COPROCESSOR_RCP:
        InitRcpMode(aRestListenAddress, aRestListenPort);
        break;
    case OT_COPROCESSOR_NCP:
        InitNcpMode();
        break;
    default:
        DieNow("Unknown coprocessor type!");
        break;
    }

#if OTBR_ENABLE_DBUS_SERVER
    mDBusAgent.Init();
#endif

    otbrLogInfo("Co-processor version: %s", mHost.GetCoprocessorVersion());
}

void Application::Deinit(void)
{
    switch (mHost.GetCoprocessorType())
    {
    case OT_COPROCESSOR_RCP:
        DeinitRcpMode();
        break;
    case OT_COPROCESSOR_NCP:
        DeinitNcpMode();
        break;
    default:
        DieNow("Unknown coprocessor type!");
        break;
    }

    mHost.Deinit();
}

otbrError Application::Run(void)
{
    otbrError error = OTBR_ERROR_NONE;

#ifdef HAVE_LIBSYSTEMD
    if (getenv("SYSTEMD_EXEC_PID") != nullptr)
    {
        otbrLogInfo("Notify systemd the service is ready.");

        // Ignored return value as systemd recommends.
        // See https://www.freedesktop.org/software/systemd/man/sd_notify.html
        sd_notify(0, "READY=1");
    }
#endif

#if OTBR_ENABLE_NOTIFY_UPSTART
    if (getenv("UPSTART_JOB") != nullptr)
    {
        otbrLogInfo("Notify Upstart the service is ready.");
        if (raise(SIGSTOP))
        {
            otbrLogWarning("Failed to notify Upstart.");
        }
    }
#endif

    // allow quitting elegantly
    signal(SIGTERM, HandleSignal);

    // avoid exiting on SIGPIPE
    signal(SIGPIPE, SIG_IGN);

    while (!sShouldTerminate)
    {
        otbr::MainloopContext mainloop;
        int                   rval;

        mainloop.mMaxFd   = -1;
        mainloop.mTimeout = kPollTimeout;

        FD_ZERO(&mainloop.mReadFdSet);
        FD_ZERO(&mainloop.mWriteFdSet);
        FD_ZERO(&mainloop.mErrorFdSet);

        MainloopManager::GetInstance().Update(mainloop);

        rval = select(mainloop.mMaxFd + 1, &mainloop.mReadFdSet, &mainloop.mWriteFdSet, &mainloop.mErrorFdSet,
                      &mainloop.mTimeout);

        if (rval >= 0)
        {
            MainloopManager::GetInstance().Process(mainloop);

            if (mErrorCondition)
            {
                error = mErrorCondition();
                if (error != OTBR_ERROR_NONE)
                {
                    break;
                }
            }
        }
        else if (errno != EINTR)
        {
            error = OTBR_ERROR_ERRNO;
            otbrLogErr("select() failed: %s", strerror(errno));
            break;
        }
    }

    return error;
}

void Application::HandleSignal(int aSignal)
{
    sShouldTerminate = true;
    signal(aSignal, SIG_DFL);
}

void Application::CreateRcpMode(void)
{
    otbr::Host::RcpHost &rcpHost = static_cast<otbr::Host::RcpHost &>(mHost);
#if OTBR_ENABLE_BACKBONE_ROUTER
    mBackboneAgent = MakeUnique<BackboneRouter::BackboneAgent>(rcpHost, mInterfaceName, mBackboneInterfaceName);
#endif
#if OTBR_ENABLE_SRP_ADVERTISING_PROXY
    mAdvertisingProxy = MakeUnique<AdvertisingProxy>(rcpHost, *mPublisher);
#endif
#if OTBR_ENABLE_DNSSD_DISCOVERY_PROXY
    mDiscoveryProxy = MakeUnique<Dnssd::DiscoveryProxy>(rcpHost, *mPublisher);
#endif
#if OTBR_ENABLE_TREL
    mTrelDnssd = MakeUnique<TrelDnssd::TrelDnssd>(rcpHost, *mPublisher);
#endif
#if OTBR_ENABLE_OPENWRT
    mUbusAgent = MakeUnique<ubus::UBusAgent>(rcpHost);
#endif
#if OTBR_ENABLE_REST_SERVER
    mRestWebServer = MakeUnique<rest::RestWebServer>(rcpHost);
#endif
#if OTBR_ENABLE_VENDOR_SERVER
    mVendorServer = vendor::VendorServer::newInstance(*this);
#endif

    OTBR_UNUSED_VARIABLE(rcpHost);
}

void Application::InitRcpMode(const std::string &aRestListenAddress, int aRestListenPort)
{
    Host::RcpHost &rcpHost = static_cast<otbr::Host::RcpHost &>(mHost);
    OTBR_UNUSED_VARIABLE(rcpHost);
    OTBR_UNUSED_VARIABLE(aRestListenAddress);
    OTBR_UNUSED_VARIABLE(aRestListenPort);

#if OTBR_ENABLE_BORDER_AGENT && OTBR_ENABLE_BORDER_AGENT_MESHCOP_SERVICE
    mMdnsStateSubject.AddObserver(mBorderAgent);
#endif
#if OTBR_ENABLE_SRP_ADVERTISING_PROXY
    mMdnsStateSubject.AddObserver(*mAdvertisingProxy);
#endif
#if OTBR_ENABLE_DNSSD_DISCOVERY_PROXY
    mMdnsStateSubject.AddObserver(*mDiscoveryProxy);
#endif
#if OTBR_ENABLE_TREL
    mMdnsStateSubject.AddObserver(*mTrelDnssd);
#endif
#if OTBR_ENABLE_DNSSD_PLAT
    mMdnsStateSubject.AddObserver(mDnssdPlatform);
    mDnssdPlatform.SetDnssdStateChangedCallback(([&rcpHost](otPlatDnssdState aState) {
        OTBR_UNUSED_VARIABLE(aState);
        otPlatDnssdStateHandleStateChange(rcpHost.GetInstance());
    }));
#endif

#if OTBR_ENABLE_MDNS
    mPublisher->Start();
#endif
#if OTBR_ENABLE_BORDER_AGENT && OTBR_ENABLE_BORDER_AGENT_MESHCOP_SERVICE
    mHost.SetBorderAgentMeshCoPServiceChangedCallback(
        [this](bool aIsActive, uint16_t aPort, const uint8_t *aTxtData, uint16_t aLength) {
            mBorderAgent.HandleBorderAgentMeshCoPServiceChanged(aIsActive, aPort,
                                                                std::vector<uint8_t>(aTxtData, aTxtData + aLength));
        });
    mHost.AddEphemeralKeyStateChangedCallback([this](otBorderAgentEphemeralKeyState aEpskcState, uint16_t aPort) {
        mBorderAgent.HandleEpskcStateChanged(aEpskcState, aPort);
    });
    SetBorderAgentOnInitState();
#endif
#if OTBR_ENABLE_BACKBONE_ROUTER
    mBackboneAgent->Init();
#endif
#if OTBR_ENABLE_SRP_ADVERTISING_PROXY
    mAdvertisingProxy->SetEnabled(true);
#endif
#if OTBR_ENABLE_DNSSD_DISCOVERY_PROXY
    mDiscoveryProxy->SetEnabled(true);
#endif
#if OTBR_ENABLE_OPENWRT
    mUbusAgent->Init();
#endif
#if OTBR_ENABLE_REST_SERVER
    mRestWebServer->Init(aRestListenAddress, aRestListenPort);
#endif
#if OTBR_ENABLE_VENDOR_SERVER
    mVendorServer->Init();
#endif
#if OTBR_ENABLE_DNSSD_PLAT
    mDnssdPlatform.Start();
#endif
}

void Application::DeinitRcpMode(void)
{
#if OTBR_ENABLE_DNSSD_PLAT
    mDnssdPlatform.Stop();
#endif
#if OTBR_ENABLE_SRP_ADVERTISING_PROXY
    mAdvertisingProxy->SetEnabled(false);
#endif
#if OTBR_ENABLE_DNSSD_DISCOVERY_PROXY
    mDiscoveryProxy->SetEnabled(false);
#endif
#if OTBR_ENABLE_BORDER_AGENT
    mBorderAgent.SetEnabled(false);
    mBorderAgent.Deinit();
#endif
#if OTBR_ENABLE_MDNS
    mMdnsStateSubject.Clear();
    mPublisher->Stop();
#endif
}

void Application::CreateNcpMode(void)
{
    otbr::Host::NcpHost &ncpHost = static_cast<otbr::Host::NcpHost &>(mHost);

    mNetif   = MakeUnique<Netif>(mInterfaceName, ncpHost);
    mInfraIf = MakeUnique<InfraIf>(ncpHost);
#if OTBR_ENABLE_BACKBONE_ROUTER
    mMulticastRoutingManager = MakeUnique<MulticastRoutingManager>(*mNetif, *mInfraIf, ncpHost);
#endif
}

void Application::InitNcpMode(void)
{
    otbr::Host::NcpHost &ncpHost = static_cast<otbr::Host::NcpHost &>(mHost);

    SuccessOrDie(mNetif->Init(), "Failed to initialize the Netif!");
    ncpHost.InitNetifCallbacks(*mNetif);

    mInfraIf->Init();
    if (!mBackboneInterfaceName.empty())
    {
        mInfraIf->SetInfraIf(mBackboneInterfaceName);
    }
    ncpHost.InitInfraIfCallbacks(*mInfraIf);

#if OTBR_ENABLE_SRP_ADVERTISING_PROXY
    ncpHost.SetMdnsPublisher(mPublisher.get());
    mMdnsStateSubject.AddObserver(ncpHost);
    mPublisher->Start();
#endif
#if OTBR_ENABLE_BORDER_AGENT
    mHost.SetBorderAgentMeshCoPServiceChangedCallback(
        [this](bool aIsActive, uint16_t aPort, const uint8_t *aTxtData, uint16_t aLength) {
            if (!aIsActive)
            {
                mBorderAgentUdpProxy.Stop();
            }
            else
            {
                mBorderAgentUdpProxy.Start(aPort);
            }
#if OTBR_ENABLE_BORDER_AGENT_MESHCOP_SERVICE
            mBorderAgent.HandleBorderAgentMeshCoPServiceChanged(aIsActive, mBorderAgentUdpProxy.GetHostPort(),
                                                                std::vector<uint8_t>(aTxtData, aTxtData + aLength));
#else
            OTBR_UNUSED_VARIABLE(aTxtData);
            OTBR_UNUSED_VARIABLE(aLength);
#endif
        });
    mHost.SetUdpForwardToHostCallback(
        [this](const uint8_t *aUdpPayload, uint16_t aLength, const otIp6Address &aPeerAddr, uint16_t aPeerPort) {
            mBorderAgentUdpProxy.SendToPeer(aUdpPayload, aLength, aPeerAddr, aPeerPort);
        });
    SetBorderAgentOnInitState();
#endif
#if OTBR_ENABLE_BACKBONE_ROUTER
    mHost.SetBackboneRouterStateChangedCallback(
        [this](otBackboneRouterState aState) { mMulticastRoutingManager->HandleStateChange(aState); });
    mHost.SetBackboneRouterMulticastListenerCallback(
        [this](otBackboneRouterMulticastListenerEvent aEvent, const Ip6Address &aAddress) {
            mMulticastRoutingManager->HandleBackboneMulticastListenerEvent(aEvent, aAddress);
        });
#if OTBR_ENABLE_BACKBONE_ROUTER_ON_INIT
    mHost.SetBackboneRouterEnabled(true);
#endif
#endif
}

void Application::DeinitNcpMode(void)
{
#if OTBR_ENABLE_BORDER_AGENT
    mBorderAgent.SetEnabled(false);
    mBorderAgent.Deinit();
    mBorderAgentUdpProxy.Stop();
#endif
#if OTBR_ENABLE_SRP_ADVERTISING_PROXY
    mPublisher->Stop();
#endif
    mNetif->Deinit();
    mInfraIf->Deinit();
}

#if OTBR_ENABLE_BORDER_AGENT
void Application::SetBorderAgentOnInitState(void)
{
    // This is for delaying publishing the MeshCoP service until the correct
    // vendor name and OUI etc. are correctly set by BorderAgent::SetMeshCopServiceValues()
#if OTBR_STOP_BORDER_AGENT_ON_INIT
    mBorderAgent.SetEnabled(false);
#else
    mBorderAgent.SetEnabled(true);
#endif
}
#endif

#if OTBR_ENABLE_DBUS_SERVER
DBus::DependentComponents Application::MakeDBusDependentComponents(void)
{
    return DBus::DependentComponents
    {
        mHost, *mPublisher,
#if OTBR_ENABLE_BORDER_AGENT
            mBorderAgent
#endif
    };
}
#endif

} // namespace otbr
