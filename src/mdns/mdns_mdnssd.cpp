/*
 *    Copyright (c) 2018, The OpenThread Authors.
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
 *   This file implements mDNS publisher based on mDNSResponder.
 */

#define OTBR_LOG_TAG "MDNS"

#include "mdns/mdns_mdnssd.hpp"

#include <algorithm>

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/code_utils.hpp"
#include "common/dns_utils.hpp"
#include "common/logging.hpp"
#include "common/time.hpp"

namespace otbr {

namespace Mdns {

static const char kDomain[] = "local.";

static otbrError DNSErrorToOtbrError(DNSServiceErrorType aError)
{
    otbrError error;

    switch (aError)
    {
    case kDNSServiceErr_NoError:
        error = OTBR_ERROR_NONE;
        break;

    case kDNSServiceErr_NoSuchKey:
    case kDNSServiceErr_NoSuchName:
    case kDNSServiceErr_NoSuchRecord:
        error = OTBR_ERROR_NOT_FOUND;
        break;

    case kDNSServiceErr_Invalid:
    case kDNSServiceErr_BadParam:
    case kDNSServiceErr_BadFlags:
    case kDNSServiceErr_BadInterfaceIndex:
        error = OTBR_ERROR_INVALID_ARGS;
        break;

    case kDNSServiceErr_NameConflict:
        error = OTBR_ERROR_DUPLICATED;
        break;

    case kDNSServiceErr_Unsupported:
        error = OTBR_ERROR_NOT_IMPLEMENTED;
        break;

    case kDNSServiceErr_ServiceNotRunning:
        error = OTBR_ERROR_INVALID_STATE;
        break;

    default:
        error = OTBR_ERROR_MDNS;
        break;
    }

    return error;
}

static const char *DNSErrorToString(DNSServiceErrorType aError)
{
    switch (aError)
    {
    case kDNSServiceErr_NoError:
        return "OK";

    case kDNSServiceErr_Unknown:
        // 0xFFFE FFFF
        return "Unknown";

    case kDNSServiceErr_NoSuchName:
        return "No Such Name";

    case kDNSServiceErr_NoMemory:
        return "No Memory";

    case kDNSServiceErr_BadParam:
        return "Bad Param";

    case kDNSServiceErr_BadReference:
        return "Bad Reference";

    case kDNSServiceErr_BadState:
        return "Bad State";

    case kDNSServiceErr_BadFlags:
        return "Bad Flags";

    case kDNSServiceErr_Unsupported:
        return "Unsupported";

    case kDNSServiceErr_NotInitialized:
        return "Not Initialized";

    case kDNSServiceErr_AlreadyRegistered:
        return "Already Registered";

    case kDNSServiceErr_NameConflict:
        return "Name Conflict";

    case kDNSServiceErr_Invalid:
        return "Invalid";

    case kDNSServiceErr_Firewall:
        return "Firewall";

    case kDNSServiceErr_Incompatible:
        // client library incompatible with daemon
        return "Incompatible";

    case kDNSServiceErr_BadInterfaceIndex:
        return "Bad Interface Index";

    case kDNSServiceErr_Refused:
        return "Refused";

    case kDNSServiceErr_NoSuchRecord:
        return "No Such Record";

    case kDNSServiceErr_NoAuth:
        return "No Auth";

    case kDNSServiceErr_NoSuchKey:
        return "No Such Key";

    case kDNSServiceErr_NATTraversal:
        return "NAT Traversal";

    case kDNSServiceErr_DoubleNAT:
        return "Double NAT";

    case kDNSServiceErr_BadTime:
        // Codes up to here existed in Tiger
        return "Bad Time";

    case kDNSServiceErr_BadSig:
        return "Bad Sig";

    case kDNSServiceErr_BadKey:
        return "Bad Key";

    case kDNSServiceErr_Transient:
        return "Transient";

    case kDNSServiceErr_ServiceNotRunning:
        // Background daemon not running
        return "Service Not Running";

    case kDNSServiceErr_NATPortMappingUnsupported:
        // NAT doesn't support NAT-PMP or UPnP
        return "NAT Port Mapping Unsupported";

    case kDNSServiceErr_NATPortMappingDisabled:
        // NAT supports NAT-PMP or UPnP but it's disabled by the administrator
        return "NAT Port Mapping Disabled";

    case kDNSServiceErr_NoRouter:
        // No router currently configured (probably no network connectivity)
        return "No Router";

    case kDNSServiceErr_PollingMode:
        return "Polling Mode";

    case kDNSServiceErr_Timeout:
        return "Timeout";

    default:
        assert(false);
        return nullptr;
    }
}

PublisherMDnsSd::PublisherMDnsSd(StateCallback aCallback)
    : mHostsAndKeysRef(nullptr)
    , mState(State::kIdle)
    , mStateCallback(std::move(aCallback))
{
}

PublisherMDnsSd::~PublisherMDnsSd(void)
{
    Stop();
}

otbrError PublisherMDnsSd::Start(void)
{
    mState = State::kReady;
    mStateCallback(State::kReady);
    return OTBR_ERROR_NONE;
}

bool PublisherMDnsSd::IsStarted(void) const
{
    return mState == State::kReady;
}

void PublisherMDnsSd::Stop(void)
{
    ServiceRegistrationMap serviceRegistrations;
    HostRegistrationMap    hostRegistrations;
    KeyRegistrationMap     keyRegistrations;

    VerifyOrExit(mState == State::kReady);

    std::swap(mServiceRegistrations, serviceRegistrations);
    std::swap(mHostRegistrations, hostRegistrations);
    std::swap(mKeyRegistrations, keyRegistrations);

    if (mHostsAndKeysRef != nullptr)
    {
        DNSServiceRefDeallocate(mHostsAndKeysRef);
        otbrLogDebug("Deallocated DNSServiceRef for hosts and keys: %p", mHostsAndKeysRef);
        mHostsAndKeysRef = nullptr;
    }

    mSubscribedServices.clear();

    mSubscribedHosts.clear();

    mState = State::kIdle;

exit:
    return;
}

void PublisherMDnsSd::Update(MainloopContext &aMainloop)
{
    /*
    for (auto &kv : mServiceRegistrations)
    {
        auto &serviceReg = static_cast<DnssdServiceRegistration &>(*kv.second);

        assert(serviceReg.GetServiceRef() != nullptr);

        int fd = DNSServiceRefSockFD(serviceReg.GetServiceRef());

        if (fd != -1)
        {
            FD_SET(fd, &aMainloop.mReadFdSet);
            aMainloop.mMaxFd = std::max(aMainloop.mMaxFd, fd);
        }
    }
    */

    if (mHostsAndKeysRef != nullptr)
    {
        int fd = DNSServiceRefSockFD(mHostsAndKeysRef);

        assert(fd != -1);

        FD_SET(fd, &aMainloop.mReadFdSet);

        aMainloop.mMaxFd = std::max(aMainloop.mMaxFd, fd);
    }

    for (const auto &service : mSubscribedServices)
    {
        service->UpdateAll(aMainloop);
    }

    for (const auto &host : mSubscribedHosts)
    {
        host->Update(aMainloop);
    }
}

void PublisherMDnsSd::Process(const MainloopContext &aMainloop)
{
    std::vector<DNSServiceRef> readyServices;

    /*

    for (auto &kv : mServiceRegistrations)
    {
        auto &serviceReg = static_cast<DnssdServiceRegistration &>(*kv.second);
        int   fd         = DNSServiceRefSockFD(serviceReg.GetServiceRef());

        if (FD_ISSET(fd, &aMainloop.mReadFdSet))
        {
            readyServices.push_back(serviceReg.GetServiceRef());
        }
    }
    */

    if (mHostsAndKeysRef != nullptr)
    {
        int fd = DNSServiceRefSockFD(mHostsAndKeysRef);

        if (FD_ISSET(fd, &aMainloop.mReadFdSet))
        {
            readyServices.push_back(mHostsAndKeysRef);
        }
    }

    for (const auto &service : mSubscribedServices)
    {
        service->ProcessAll(aMainloop, readyServices);
    }

    for (const auto &host : mSubscribedHosts)
    {
        host->Process(aMainloop, readyServices);
    }

    for (DNSServiceRef serviceRef : readyServices)
    {
        DNSServiceErrorType error = DNSServiceProcessResult(serviceRef);

        if (error != kDNSServiceErr_NoError)
        {
            otbrLogLevel logLevel = (error == kDNSServiceErr_BadReference) ? OTBR_LOG_INFO : OTBR_LOG_WARNING;
            otbrLog(logLevel, OTBR_LOG_TAG, "DNSServiceProcessResult failed: %s (serviceRef = %p)",
                    DNSErrorToString(error), serviceRef);
        }
        if (error == kDNSServiceErr_ServiceNotRunning)
        {
            otbrLogWarning("Need to reconnect to mdnsd");
            Stop();
            Start();
            ExitNow();
        }
    }
exit:
    return;
}

PublisherMDnsSd::DnssdServiceRegistration::~DnssdServiceRegistration(void)
{
    if (mServiceRef != nullptr)
    {
        DNSServiceRefDeallocate(mServiceRef);
    }
}

PublisherMDnsSd::DnssdHostRegistration::~DnssdHostRegistration(void)
{
    int dnsError;

    VerifyOrExit(mServiceRef != nullptr);

    for (const auto &recordRefAndAddress : GetRecordRefMap())
    {
        const DNSRecordRef &recordRef = recordRefAndAddress.first;
        const Ip6Address   &address   = recordRefAndAddress.second;
        if (IsCompleted())
        {
            // The Bonjour mDNSResponder somehow doesn't send goodbye message for the AAAA record when it is
            // removed by `DNSServiceRemoveRecord`. Per RFC 6762, a goodbye message of a record sets its TTL
            // to zero but the receiver should record the TTL of 1 and flushes the cache 1 second later. Here
            // we remove the AAAA record after updating its TTL to 1 second. This has the same effect as
            // sending a goodbye message.
            // TODO: resolve the goodbye issue with Bonjour mDNSResponder.
            dnsError = DNSServiceUpdateRecord(mServiceRef, recordRef, kDNSServiceFlagsUnique, sizeof(address.m8),
                                              address.m8, /* ttl */ 1);
            otbrLogResult(DNSErrorToOtbrError(dnsError), "Send goodbye message for host %s address %s: %s",
                          MakeFullHostName(mName).c_str(), address.ToString().c_str(), DNSErrorToString(dnsError));
        }
        dnsError = DNSServiceRemoveRecord(mServiceRef, recordRef, /* flags */ 0);
        otbrLogResult(DNSErrorToOtbrError(dnsError), "Remove record for host %s address %s: %s",
                      MakeFullHostName(mName).c_str(), address.ToString().c_str(), DNSErrorToString(dnsError));
        // TODO: ?
        // DNSRecordRefDeallocate(recordRef);
    }

exit:
    return;
}

PublisherMDnsSd::DnssdKeyRegistration::~DnssdKeyRegistration(void)
{
    int dnsError;

    VerifyOrExit(mServiceRef != nullptr);

    if (IsCompleted())
    {
        // Send goodbye message (see comment in `~DnssdHostRegistration`)
        dnsError = DNSServiceUpdateRecord(mServiceRef, mRecordRef, kDNSServiceFlagsUnique, mKeyData.size(),
                                          mKeyData.data(), /* ttl */ 1);
        otbrLogResult(DNSErrorToOtbrError(dnsError), "Send goodbye message for key %s: %s", mName.c_str(),
                      DNSErrorToString(dnsError));
    }

    dnsError = DNSServiceRemoveRecord(mServiceRef, mRecordRef, /* flags */ 0);

    otbrLogResult(DNSErrorToOtbrError(dnsError), "Remove key record for %s: %s", mName.c_str(),
                  DNSErrorToString(dnsError));

exit:
    return;
}

Publisher::ServiceRegistration *PublisherMDnsSd::FindServiceRegistration(const DNSServiceRef &aServiceRef)
{
    ServiceRegistration *result = nullptr;

    for (auto &kv : mServiceRegistrations)
    {
        // We are sure that the service registrations must be instances of `DnssdServiceRegistration`.
        auto &serviceReg = static_cast<DnssdServiceRegistration &>(*kv.second);

        if (serviceReg.GetServiceRef() == aServiceRef)
        {
            result = kv.second.get();
            break;
        }
    }

    return result;
}

Publisher::HostRegistration *PublisherMDnsSd::FindHostRegistration(const DNSServiceRef &aServiceRef,
                                                                   const DNSRecordRef  &aRecordRef)
{
    HostRegistration *result = nullptr;

    for (auto &kv : mHostRegistrations)
    {
        // We are sure that the host registrations must be instances of `DnssdHostRegistration`.
        auto &hostReg = static_cast<DnssdHostRegistration &>(*kv.second);

        if (hostReg.GetServiceRef() == aServiceRef && hostReg.GetRecordRefMap().count(aRecordRef))
        {
            result = kv.second.get();
            break;
        }
    }

    return result;
}

Publisher::KeyRegistration *PublisherMDnsSd::FindKeyRegistration(const DNSServiceRef &aServiceRef,
                                                                 const DNSRecordRef  &aRecordRef)
{
    KeyRegistration *result = nullptr;

    for (auto &entry : mKeyRegistrations)
    {
        auto &keyReg = static_cast<DnssdKeyRegistration &>(*entry.second);

        if (keyReg.GetServiceRef() == aServiceRef && keyReg.GetRecordRef() == aRecordRef)
        {
            result = entry.second.get();
            break;
        }
    }

    return result;
}

void PublisherMDnsSd::HandleServiceRegisterResult(DNSServiceRef         aService,
                                                  const DNSServiceFlags aFlags,
                                                  DNSServiceErrorType   aError,
                                                  const char           *aName,
                                                  const char           *aType,
                                                  const char           *aDomain,
                                                  void                 *aContext)
{
    static_cast<PublisherMDnsSd *>(aContext)->HandleServiceRegisterResult(aService, aFlags, aError, aName, aType,
                                                                          aDomain);
}

void PublisherMDnsSd::HandleServiceRegisterResult(DNSServiceRef         aServiceRef,
                                                  const DNSServiceFlags aFlags,
                                                  DNSServiceErrorType   aError,
                                                  const char           *aName,
                                                  const char           *aType,
                                                  const char           *aDomain)
{
    OTBR_UNUSED_VARIABLE(aDomain);

    otbrError            error      = DNSErrorToOtbrError(aError);
    ServiceRegistration *serviceReg = FindServiceRegistration(aServiceRef);
    serviceReg->mName               = aName;

    otbrLogInfo("Received reply for service %s.%s, serviceRef = %p, flags=0x%x", aName, aType, aServiceRef, aFlags);
    otbrLogInfo("flags=0x%x, aError=%u, domain=%s", aFlags, aError, aDomain);

    VerifyOrExit(serviceReg != nullptr);

    if (aError == kDNSServiceErr_NoError && (aFlags & kDNSServiceFlagsAdd))
    {
        otbrLogInfo("Successfully registered service %s.%s", aName, aType);
        serviceReg->Complete(OTBR_ERROR_NONE);
    }
    else
    {
        otbrLogErr("Failed to register service %s.%s: %s", aName, aType, DNSErrorToString(aError));
        RemoveServiceRegistration(serviceReg->mName, serviceReg->mType, error);
        OTBR_UNUSED_VARIABLE(error);
    }

exit:
    return;
}

otbrError PublisherMDnsSd::PublishServiceImpl(const std::string &aHostName,
                                              const std::string &aName,
                                              const std::string &aType,
                                              const SubTypeList &aSubTypeList,
                                              uint16_t           aPort,
                                              const TxtData     &aTxtData,
                                              ResultCallback   &&aCallback)
{
    otbrError     ret               = OTBR_ERROR_NONE;
    int           error             = 0;
    SubTypeList   sortedSubTypeList = SortSubTypeList(aSubTypeList);
    std::string   regType           = MakeRegType(aType, sortedSubTypeList);
    DNSServiceRef serviceRef        = nullptr;
    std::string   fullHostName;
    const char   *hostNameCString    = nullptr;
    const char   *serviceNameCString = nullptr;

    VerifyOrExit(mState == State::kReady, ret = OTBR_ERROR_INVALID_STATE);

    if (!aHostName.empty())
    {
        fullHostName    = MakeFullHostName(aHostName);
        hostNameCString = fullHostName.c_str();
    }
    if (!aName.empty())
    {
        serviceNameCString = aName.c_str();
    }

    aCallback = HandleDuplicateServiceRegistration(aHostName, aName, aType, sortedSubTypeList, aPort, aTxtData,
                                                   std::move(aCallback));
    VerifyOrExit(!aCallback.IsNull());

    SuccessOrExit(error = AllocateHostsAndKeysRefIfUnallocated());

    serviceRef = mHostsAndKeysRef;
    otbrLogInfo("Registering new service %s.%s.local, serviceRef = %p", aName.c_str(), regType.c_str(), serviceRef);

    SuccessOrExit(error = DNSServiceRegister(&serviceRef,
                                              kDNSServiceFlagsNoAutoRename | kDNSServiceFlagsShareConnection | kDNSServiceFlagsShared,
                                              kDNSServiceInterfaceIndexAny,
                                             serviceNameCString, regType.c_str(),
                                             /* domain */ nullptr, hostNameCString, htons(aPort), aTxtData.size(),
                                             aTxtData.data(), HandleServiceRegisterResult, this));

    otbrLogInfo("Registered new service %s.%s.local, serviceRef = %p", aName.c_str(), regType.c_str(), serviceRef);
    AddServiceRegistration(std::unique_ptr<DnssdServiceRegistration>(new DnssdServiceRegistration(
        aHostName, aName, aType, sortedSubTypeList, aPort, aTxtData, std::move(aCallback), serviceRef, this)));

exit:
    if (error != kDNSServiceErr_NoError || ret != OTBR_ERROR_NONE)
    {
        if (error != kDNSServiceErr_NoError)
        {
            ret = DNSErrorToOtbrError(error);
            otbrLogErr("Failed to publish service %s.%s for mdnssd error: %s!", aName.c_str(), aType.c_str(),
                       DNSErrorToString(error));
        }

        if (serviceRef != nullptr)
        {
            DNSServiceRefDeallocate(serviceRef);
        }
        std::move(aCallback)(ret);
    }
    return ret;
}

void PublisherMDnsSd::UnpublishService(const std::string &aName, const std::string &aType, ResultCallback &&aCallback)
{
    otbrError error = OTBR_ERROR_NONE;

    VerifyOrExit(mState == Publisher::State::kReady, error = OTBR_ERROR_INVALID_STATE);
    RemoveServiceRegistration(aName, aType, OTBR_ERROR_ABORTED);

exit:
    std::move(aCallback)(error);
}

int PublisherMDnsSd::AllocateHostsAndKeysRefIfUnallocated(void)
{
    int dnsError = kDNSServiceErr_NoError;

    VerifyOrExit(mHostsAndKeysRef == nullptr);

    SuccessOrExit(dnsError = DNSServiceCreateConnection(&mHostsAndKeysRef));
    otbrLogInfo("Created new DNSServiceRef for hosts and keys: %p", mHostsAndKeysRef);
    //otbrLogInfo("ABTIN -> mHostsAndKeysRef->primary = %p", mHostsAndKeysRef->primary);

exit:
    return dnsError;
}

otbrError PublisherMDnsSd::PublishHostImpl(const std::string             &aName,
                                           const std::vector<Ip6Address> &aAddresses,
                                           ResultCallback               &&aCallback)
{
    otbrError              ret   = OTBR_ERROR_NONE;
    int                    error = 0;
    std::string            fullName;
    DnssdHostRegistration *registration;

    VerifyOrExit(mState == Publisher::State::kReady, ret = OTBR_ERROR_INVALID_STATE);

    fullName = MakeFullHostName(aName);

    aCallback = HandleDuplicateHostRegistration(aName, aAddresses, std::move(aCallback));
    VerifyOrExit(!aCallback.IsNull());
    VerifyOrExit(!aAddresses.empty(), std::move(aCallback)(OTBR_ERROR_NONE));

    SuccessOrExit(error = AllocateHostsAndKeysRefIfUnallocated());

    registration = new DnssdHostRegistration(aName, aAddresses, std::move(aCallback), mHostsAndKeysRef, this);

    otbrLogInfo("Registering new host %s", fullName.c_str());
    for (const auto &address : aAddresses)
    {
        DNSRecordRef recordRef = nullptr;
        // Supports only IPv6 for now, may support IPv4 in the future.
        SuccessOrExit(error = DNSServiceRegisterRecord(mHostsAndKeysRef, &recordRef, kDNSServiceFlagsShared | kDNSServiceFlagsShareConnection,
                                                       kDNSServiceInterfaceIndexAny, fullName.c_str(),
                                                       kDNSServiceType_AAAA, kDNSServiceClass_IN, sizeof(address.m8),
                                                       address.m8, /* ttl */ 0, HandleRegisterHostResult, this));
        registration->GetRecordRefMap()[recordRef] = address;
    }

    AddHostRegistration(std::unique_ptr<DnssdHostRegistration>(registration));

exit:
    if (error != kDNSServiceErr_NoError || ret != OTBR_ERROR_NONE)
    {
        if (error != kDNSServiceErr_NoError)
        {
            ret = DNSErrorToOtbrError(error);
            otbrLogErr("Failed to publish/update host %s for mdnssd error: %s!", aName.c_str(),
                       DNSErrorToString(error));
        }

        std::move(aCallback)(ret);
    }
    return ret;
}

void PublisherMDnsSd::UnpublishHost(const std::string &aName, ResultCallback &&aCallback)
{
    otbrError error = OTBR_ERROR_NONE;

    VerifyOrExit(mState == Publisher::State::kReady, error = OTBR_ERROR_INVALID_STATE);
    RemoveHostRegistration(aName, OTBR_ERROR_ABORTED);

exit:
    // We may failed to unregister the host from underlying mDNS publishers, but
    // it usually means that the mDNS publisher is already not functioning. So it's
    // okay to return success directly since the service is not advertised anyway.
    std::move(aCallback)(error);
}

void PublisherMDnsSd::HandleRegisterHostResult(DNSServiceRef       aServiceRef,
                                               DNSRecordRef        aRecordRef,
                                               DNSServiceFlags     aFlags,
                                               DNSServiceErrorType aError,
                                               void               *aContext)
{
    static_cast<PublisherMDnsSd *>(aContext)->HandleRegisterHostResult(aServiceRef, aRecordRef, aFlags, aError);
}

void PublisherMDnsSd::HandleRegisterHostResult(DNSServiceRef       aServiceRef,
                                               DNSRecordRef        aRecordRef,
                                               DNSServiceFlags     aFlags,
                                               DNSServiceErrorType aError)
{
    OTBR_UNUSED_VARIABLE(aFlags);

    otbrError error   = DNSErrorToOtbrError(aError);
    auto     *hostReg = static_cast<DnssdHostRegistration *>(FindHostRegistration(aServiceRef, aRecordRef));

    std::string hostName;

    VerifyOrExit(hostReg != nullptr);

    hostName = MakeFullHostName(hostReg->mName);

    otbrLogInfo("Received reply for host %s: %s", hostName.c_str(), DNSErrorToString(aError));

    if (error == OTBR_ERROR_NONE)
    {
        --hostReg->mCallbackCount;
        if (!hostReg->mCallbackCount)
        {
            otbrLogInfo("Successfully registered host %s", hostName.c_str());
            hostReg->Complete(OTBR_ERROR_NONE);
        }
    }
    else
    {
        otbrLogWarning("Failed to register host %s for mdnssd error: %s", hostName.c_str(), DNSErrorToString(aError));
        RemoveHostRegistration(hostReg->mName, error);
    }

exit:
    return;
}

otbrError PublisherMDnsSd::PublishKeyImpl(const std::string &aName, const KeyData &aKeyData, ResultCallback &&aCallback)
{
    otbrError    ret   = OTBR_ERROR_NONE;
    int          error = 0;
    std::string  fullName;
    DNSRecordRef recordRef = nullptr;
    DnssdServiceRegistration *serviceReg;

    VerifyOrExit(mState == Publisher::State::kReady, ret = OTBR_ERROR_INVALID_STATE);

    fullName = MakeFullKeyName(aName);

    aCallback = HandleDuplicateKeyRegistration(aName, aKeyData, std::move(aCallback));
    VerifyOrExit(!aCallback.IsNull());

    otbrLogInfo("Registering new key %s", fullName.c_str());

    SuccessOrExit(error = AllocateHostsAndKeysRefIfUnallocated());

    serviceReg = static_cast<DnssdServiceRegistration *>(Publisher::FindServiceRegistration(fullName));

    if (serviceReg != nullptr)
    {
        otbrLogInfo("Found matching service reg for key");

        // mServiceRef
        error = DNSServiceAddRecord(serviceReg->mServiceRef, &recordRef,
            kDNSServiceFlagsShared, kDNSServiceType_KEY, aKeyData.size(), aKeyData.data(), 0);
    }
    else
    {
        error = DNSServiceRegisterRecord(mHostsAndKeysRef, &recordRef,
                                                   kDNSServiceFlagsUnique,
                                                   kDNSServiceInterfaceIndexAny, fullName.c_str(), kDNSServiceType_KEY,
                                                   kDNSServiceClass_IN, aKeyData.size(), aKeyData.data(), /* ttl */ 0,
                                                   HandleRegisterKeyResult, this);
    }

    SuccessOrExit(error);

    AddKeyRegistration(std::unique_ptr<DnssdKeyRegistration>(
        new DnssdKeyRegistration(aName, aKeyData, std::move(aCallback), mHostsAndKeysRef, recordRef, this)));

exit:
    if (error != kDNSServiceErr_NoError || ret != OTBR_ERROR_NONE)
    {
        if (error != kDNSServiceErr_NoError)
        {
            ret = DNSErrorToOtbrError(error);
            otbrLogErr("Failed to publish/update key for %s mdnssd error: %s!", aName.c_str(), DNSErrorToString(error));
        }

        std::move(aCallback)(ret);
    }
    return ret;
}

void PublisherMDnsSd::UnpublishKey(const std::string &aName, ResultCallback &&aCallback)
{
    otbrError error = OTBR_ERROR_NONE;

    VerifyOrExit(mState == Publisher::State::kReady, error = OTBR_ERROR_INVALID_STATE);
    RemoveKeyRegistration(aName, OTBR_ERROR_ABORTED);

exit:
    std::move(aCallback)(error);
}

void PublisherMDnsSd::HandleRegisterKeyResult(DNSServiceRef       aServiceRef,
                                              DNSRecordRef        aRecordRef,
                                              DNSServiceFlags     aFlags,
                                              DNSServiceErrorType aError,
                                              void               *aContext)
{
    static_cast<PublisherMDnsSd *>(aContext)->HandleRegisterKeyResult(aServiceRef, aRecordRef, aFlags, aError);
}

void PublisherMDnsSd::HandleRegisterKeyResult(DNSServiceRef       aServiceRef,
                                              DNSRecordRef        aRecordRef,
                                              DNSServiceFlags     aFlags,
                                              DNSServiceErrorType aError)
{
    OTBR_UNUSED_VARIABLE(aFlags);

    otbrError   error  = DNSErrorToOtbrError(aError);
    auto       *keyReg = static_cast<DnssdKeyRegistration *>(FindKeyRegistration(aServiceRef, aRecordRef));
    std::string keyName;

    VerifyOrExit(keyReg != nullptr);

    keyName = MakeFullKeyName(keyReg->mName);

    if (error == OTBR_ERROR_NONE)
    {
        otbrLogInfo("Successfully registered key for %s", keyName.c_str());
        keyReg->Complete(OTBR_ERROR_NONE);
    }
    else
    {
        otbrLogWarning("Failed to register key for %s - mdnssd error: %s", keyName.c_str(), DNSErrorToString(aError));
        RemoveKeyRegistration(keyReg->mName, error);
    }

exit:
    return;
}

// See `regtype` parameter of the DNSServiceRegister() function for more information.
std::string PublisherMDnsSd::MakeRegType(const std::string &aType, SubTypeList aSubTypeList)
{
    std::string regType = aType;

    std::sort(aSubTypeList.begin(), aSubTypeList.end());

    for (const auto &subType : aSubTypeList)
    {
        regType += "," + subType;
    }

    return regType;
}

void PublisherMDnsSd::SubscribeService(const std::string &aType, const std::string &aInstanceName)
{
    VerifyOrExit(mState == Publisher::State::kReady);
    mSubscribedServices.push_back(MakeUnique<ServiceSubscription>(*this, aType, aInstanceName));

    otbrLogInfo("Subscribe service %s.%s (total %zu)", aInstanceName.c_str(), aType.c_str(),
                mSubscribedServices.size());

    if (aInstanceName.empty())
    {
        mSubscribedServices.back()->Browse();
    }
    else
    {
        mSubscribedServices.back()->Resolve(kDNSServiceInterfaceIndexAny, aInstanceName, aType, kDomain);
    }

exit:
    return;
}

void PublisherMDnsSd::UnsubscribeService(const std::string &aType, const std::string &aInstanceName)
{
    ServiceSubscriptionList::iterator it;

    VerifyOrExit(mState == Publisher::State::kReady);
    it = std::find_if(mSubscribedServices.begin(), mSubscribedServices.end(),
                      [&aType, &aInstanceName](const std::unique_ptr<ServiceSubscription> &aService) {
                          return aService->mType == aType && aService->mInstanceName == aInstanceName;
                      });
    assert(it != mSubscribedServices.end());

    mSubscribedServices.erase(it);

    otbrLogInfo("Unsubscribe service %s.%s (left %zu)", aInstanceName.c_str(), aType.c_str(),
                mSubscribedServices.size());

exit:
    return;
}

void PublisherMDnsSd::OnServiceResolveFailedImpl(const std::string &aType,
                                                 const std::string &aInstanceName,
                                                 int32_t            aErrorCode)
{
    otbrLogWarning("Resolve service %s.%s failed: code=%" PRId32, aInstanceName.c_str(), aType.c_str(), aErrorCode);
}

void PublisherMDnsSd::OnHostResolveFailedImpl(const std::string &aHostName, int32_t aErrorCode)
{
    otbrLogWarning("Resolve host %s failed: code=%" PRId32, aHostName.c_str(), aErrorCode);
}

otbrError PublisherMDnsSd::DnsErrorToOtbrError(int32_t aErrorCode)
{
    return otbr::Mdns::DNSErrorToOtbrError(aErrorCode);
}

void PublisherMDnsSd::SubscribeHost(const std::string &aHostName)
{
    VerifyOrExit(mState == State::kReady);
    mSubscribedHosts.push_back(MakeUnique<HostSubscription>(*this, aHostName));

    otbrLogInfo("Subscribe host %s (total %zu)", aHostName.c_str(), mSubscribedHosts.size());

    mSubscribedHosts.back()->Resolve();

exit:
    return;
}

void PublisherMDnsSd::UnsubscribeHost(const std::string &aHostName)
{
    HostSubscriptionList ::iterator it;

    VerifyOrExit(mState == Publisher::State::kReady);
    it = std::find_if(
        mSubscribedHosts.begin(), mSubscribedHosts.end(),
        [&aHostName](const std::unique_ptr<HostSubscription> &aHost) { return aHost->mHostName == aHostName; });

    assert(it != mSubscribedHosts.end());

    mSubscribedHosts.erase(it);

    otbrLogInfo("Unsubscribe host %s (remaining %d)", aHostName.c_str(), mSubscribedHosts.size());

exit:
    return;
}

Publisher *Publisher::Create(StateCallback aCallback)
{
    return new PublisherMDnsSd(aCallback);
}

void Publisher::Destroy(Publisher *aPublisher)
{
    delete static_cast<PublisherMDnsSd *>(aPublisher);
}

void PublisherMDnsSd::ServiceRef::Release(void)
{
    DeallocateServiceRef();
}

void PublisherMDnsSd::ServiceRef::DeallocateServiceRef(void)
{
    if (mServiceRef != nullptr)
    {
        DNSServiceRefDeallocate(mServiceRef);
        mServiceRef = nullptr;
    }
}

void PublisherMDnsSd::ServiceRef::Update(MainloopContext &aMainloop) const
{
    int fd;

    VerifyOrExit(mServiceRef != nullptr);

    fd = DNSServiceRefSockFD(mServiceRef);
    assert(fd != -1);
    FD_SET(fd, &aMainloop.mReadFdSet);
    aMainloop.mMaxFd = std::max(aMainloop.mMaxFd, fd);
exit:
    return;
}

void PublisherMDnsSd::ServiceRef::Process(const MainloopContext      &aMainloop,
                                          std::vector<DNSServiceRef> &aReadyServices) const
{
    int fd;

    VerifyOrExit(mServiceRef != nullptr);

    fd = DNSServiceRefSockFD(mServiceRef);
    assert(fd != -1);
    if (FD_ISSET(fd, &aMainloop.mReadFdSet))
    {
        aReadyServices.push_back(mServiceRef);
    }
exit:
    return;
}

void PublisherMDnsSd::ServiceSubscription::Browse(void)
{
    assert(mServiceRef == nullptr);

    otbrLogInfo("DNSServiceBrowse %s", mType.c_str());
    DNSServiceBrowse(&mServiceRef, /* flags */ 0, kDNSServiceInterfaceIndexAny, mType.c_str(),
                     /* domain */ nullptr, HandleBrowseResult, this);
}

void PublisherMDnsSd::ServiceSubscription::HandleBrowseResult(DNSServiceRef       aServiceRef,
                                                              DNSServiceFlags     aFlags,
                                                              uint32_t            aInterfaceIndex,
                                                              DNSServiceErrorType aErrorCode,
                                                              const char         *aInstanceName,
                                                              const char         *aType,
                                                              const char         *aDomain,
                                                              void               *aContext)
{
    static_cast<ServiceSubscription *>(aContext)->HandleBrowseResult(aServiceRef, aFlags, aInterfaceIndex, aErrorCode,
                                                                     aInstanceName, aType, aDomain);
}

void PublisherMDnsSd::ServiceSubscription::HandleBrowseResult(DNSServiceRef       aServiceRef,
                                                              DNSServiceFlags     aFlags,
                                                              uint32_t            aInterfaceIndex,
                                                              DNSServiceErrorType aErrorCode,
                                                              const char         *aInstanceName,
                                                              const char         *aType,
                                                              const char         *aDomain)
{
    OTBR_UNUSED_VARIABLE(aServiceRef);
    OTBR_UNUSED_VARIABLE(aDomain);

    otbrLogInfo("DNSServiceBrowse reply: %s %s.%s inf %" PRIu32 ", flags=%" PRIu32 ", error=%" PRId32,
                aFlags & kDNSServiceFlagsAdd ? "add" : "remove", aInstanceName, aType, aInterfaceIndex, aFlags,
                aErrorCode);

    VerifyOrExit(aErrorCode == kDNSServiceErr_NoError);

    if (aFlags & kDNSServiceFlagsAdd)
    {
        Resolve(aInterfaceIndex, aInstanceName, aType, aDomain);
    }
    else
    {
        mMDnsSd->OnServiceRemoved(aInterfaceIndex, mType, aInstanceName);
    }

exit:
    if (aErrorCode != kDNSServiceErr_NoError)
    {
        mMDnsSd->OnServiceResolveFailed(mType, mInstanceName, aErrorCode);
        Release();
    }
}

void PublisherMDnsSd::ServiceSubscription::Resolve(uint32_t           aInterfaceIndex,
                                                   const std::string &aInstanceName,
                                                   const std::string &aType,
                                                   const std::string &aDomain)
{
    mResolvingInstances.push_back(
        MakeUnique<ServiceInstanceResolution>(*this, aInstanceName, aType, aDomain, aInterfaceIndex));
    mResolvingInstances.back()->Resolve();
}

void PublisherMDnsSd::ServiceSubscription::RemoveInstanceResolution(
    PublisherMDnsSd::ServiceInstanceResolution &aInstanceResolution)
{
    auto it = std::find_if(mResolvingInstances.begin(), mResolvingInstances.end(),
                           [&aInstanceResolution](const std::unique_ptr<ServiceInstanceResolution> &aElem) {
                               return &aInstanceResolution == aElem.get();
                           });

    assert(it != mResolvingInstances.end());

    mResolvingInstances.erase(it);
}

void PublisherMDnsSd::ServiceSubscription::UpdateAll(MainloopContext &aMainloop) const
{
    Update(aMainloop);

    for (const auto &instance : mResolvingInstances)
    {
        instance->Update(aMainloop);
    }
}

void PublisherMDnsSd::ServiceSubscription::ProcessAll(const MainloopContext      &aMainloop,
                                                      std::vector<DNSServiceRef> &aReadyServices) const
{
    Process(aMainloop, aReadyServices);

    for (const auto &instance : mResolvingInstances)
    {
        instance->Process(aMainloop, aReadyServices);
    }
}

void PublisherMDnsSd::ServiceInstanceResolution::Resolve(void)
{
    assert(mServiceRef == nullptr);

    mSubscription->mMDnsSd->mServiceInstanceResolutionBeginTime[std::make_pair(mInstanceName, mTypeEndWithDot)] =
        Clock::now();

    otbrLogInfo("DNSServiceResolve %s %s inf %u", mInstanceName.c_str(), mTypeEndWithDot.c_str(), mNetifIndex);
    DNSServiceResolve(&mServiceRef, /* flags */ kDNSServiceFlagsTimeout, mNetifIndex, mInstanceName.c_str(),
                      mTypeEndWithDot.c_str(), mDomain.c_str(), HandleResolveResult, this);
}

void PublisherMDnsSd::ServiceInstanceResolution::HandleResolveResult(DNSServiceRef        aServiceRef,
                                                                     DNSServiceFlags      aFlags,
                                                                     uint32_t             aInterfaceIndex,
                                                                     DNSServiceErrorType  aErrorCode,
                                                                     const char          *aFullName,
                                                                     const char          *aHostTarget,
                                                                     uint16_t             aPort,
                                                                     uint16_t             aTxtLen,
                                                                     const unsigned char *aTxtRecord,
                                                                     void                *aContext)
{
    static_cast<ServiceInstanceResolution *>(aContext)->HandleResolveResult(
        aServiceRef, aFlags, aInterfaceIndex, aErrorCode, aFullName, aHostTarget, aPort, aTxtLen, aTxtRecord);
}

void PublisherMDnsSd::ServiceInstanceResolution::HandleResolveResult(DNSServiceRef        aServiceRef,
                                                                     DNSServiceFlags      aFlags,
                                                                     uint32_t             aInterfaceIndex,
                                                                     DNSServiceErrorType  aErrorCode,
                                                                     const char          *aFullName,
                                                                     const char          *aHostTarget,
                                                                     uint16_t             aPort,
                                                                     uint16_t             aTxtLen,
                                                                     const unsigned char *aTxtRecord)
{
    OTBR_UNUSED_VARIABLE(aServiceRef);

    std::string instanceName, type, domain;
    otbrError   error = OTBR_ERROR_NONE;

    otbrLogInfo("DNSServiceResolve reply: %s host %s:%d, TXT=%dB inf %u, flags=%u", aFullName, aHostTarget, aPort,
                aTxtLen, aInterfaceIndex, aFlags);

    VerifyOrExit(aErrorCode == kDNSServiceErr_NoError);

    SuccessOrExit(error = SplitFullServiceInstanceName(aFullName, instanceName, type, domain));

    mInstanceInfo.mNetifIndex = aInterfaceIndex;
    mInstanceInfo.mName       = instanceName;
    mInstanceInfo.mHostName   = aHostTarget;
    mInstanceInfo.mPort       = ntohs(aPort);
    mInstanceInfo.mTxtData.assign(aTxtRecord, aTxtRecord + aTxtLen);
    // priority and weight are not given in the reply
    mInstanceInfo.mPriority = 0;
    mInstanceInfo.mWeight   = 0;

    DeallocateServiceRef();
    error = GetAddrInfo(aInterfaceIndex);

exit:
    if (error != OTBR_ERROR_NONE)
    {
        otbrLogWarning("Failed to resolve service instance %s", aFullName);
    }

    if (aErrorCode != kDNSServiceErr_NoError || error != OTBR_ERROR_NONE)
    {
        mSubscription->mMDnsSd->OnServiceResolveFailed(mSubscription->mType, mInstanceName, aErrorCode);
        FinishResolution();
    }
}

otbrError PublisherMDnsSd::ServiceInstanceResolution::GetAddrInfo(uint32_t aInterfaceIndex)
{
    DNSServiceErrorType dnsError;

    assert(mServiceRef == nullptr);

    otbrLogInfo("DNSServiceGetAddrInfo %s inf %d", mInstanceInfo.mHostName.c_str(), aInterfaceIndex);

    dnsError = DNSServiceGetAddrInfo(&mServiceRef, kDNSServiceFlagsTimeout, aInterfaceIndex,
                                     kDNSServiceProtocol_IPv6 | kDNSServiceProtocol_IPv4,
                                     mInstanceInfo.mHostName.c_str(), HandleGetAddrInfoResult, this);

    if (dnsError != kDNSServiceErr_NoError)
    {
        otbrLogWarning("DNSServiceGetAddrInfo failed: %s", DNSErrorToString(dnsError));
    }

    return dnsError == kDNSServiceErr_NoError ? OTBR_ERROR_NONE : OTBR_ERROR_MDNS;
}

void PublisherMDnsSd::ServiceInstanceResolution::HandleGetAddrInfoResult(DNSServiceRef          aServiceRef,
                                                                         DNSServiceFlags        aFlags,
                                                                         uint32_t               aInterfaceIndex,
                                                                         DNSServiceErrorType    aErrorCode,
                                                                         const char            *aHostName,
                                                                         const struct sockaddr *aAddress,
                                                                         uint32_t               aTtl,
                                                                         void                  *aContext)
{
    static_cast<ServiceInstanceResolution *>(aContext)->HandleGetAddrInfoResult(aServiceRef, aFlags, aInterfaceIndex,
                                                                                aErrorCode, aHostName, aAddress, aTtl);
}

void PublisherMDnsSd::ServiceInstanceResolution::HandleGetAddrInfoResult(DNSServiceRef          aServiceRef,
                                                                         DNSServiceFlags        aFlags,
                                                                         uint32_t               aInterfaceIndex,
                                                                         DNSServiceErrorType    aErrorCode,
                                                                         const char            *aHostName,
                                                                         const struct sockaddr *aAddress,
                                                                         uint32_t               aTtl)
{
    OTBR_UNUSED_VARIABLE(aServiceRef);
    OTBR_UNUSED_VARIABLE(aInterfaceIndex);

    Ip6Address address;

    otbrLog(aErrorCode == kDNSServiceErr_NoError ? OTBR_LOG_INFO : OTBR_LOG_WARNING, OTBR_LOG_TAG,
            "DNSServiceGetAddrInfo reply: flags=%" PRIu32 ", host=%s, sa_family=%u, error=%" PRId32, aFlags, aHostName,
            static_cast<unsigned int>(aAddress->sa_family), aErrorCode);

    VerifyOrExit(aErrorCode == kDNSServiceErr_NoError);
    VerifyOrExit((aFlags & kDNSServiceFlagsAdd) && aAddress->sa_family == AF_INET6);

    address.CopyFrom(*reinterpret_cast<const struct sockaddr_in6 *>(aAddress));
    VerifyOrExit(!address.IsUnspecified() && !address.IsLinkLocal() && !address.IsMulticast() && !address.IsLoopback(),
                 otbrLogDebug("DNSServiceGetAddrInfo ignores address %s", address.ToString().c_str()));

    mInstanceInfo.mAddresses.push_back(address);
    mInstanceInfo.mTtl = aTtl;

    otbrLogInfo("DNSServiceGetAddrInfo reply: address=%s, ttl=%" PRIu32, address.ToString().c_str(), aTtl);

exit:
    if (!mInstanceInfo.mAddresses.empty() || aErrorCode != kDNSServiceErr_NoError)
    {
        FinishResolution();
    }
}

void PublisherMDnsSd::ServiceInstanceResolution::FinishResolution(void)
{
    ServiceSubscription   *subscription = mSubscription;
    std::string            serviceName  = mSubscription->mType;
    DiscoveredInstanceInfo instanceInfo = mInstanceInfo;

    // NOTE: `RemoveInstanceResolution` will free this `ServiceInstanceResolution` object.
    //       So, We can't access `mSubscription` after `RemoveInstanceResolution`.
    subscription->RemoveInstanceResolution(*this);

    // NOTE: The `ServiceSubscription` object may be freed in `OnServiceResolved`.
    subscription->mMDnsSd->OnServiceResolved(serviceName, instanceInfo);
}

void PublisherMDnsSd::HostSubscription::Resolve(void)
{
    std::string fullHostName = MakeFullHostName(mHostName);

    assert(mServiceRef == nullptr);

    mMDnsSd->mHostResolutionBeginTime[mHostName] = Clock::now();

    otbrLogInfo("DNSServiceGetAddrInfo %s inf %d", fullHostName.c_str(), kDNSServiceInterfaceIndexAny);

    DNSServiceGetAddrInfo(&mServiceRef, /* flags */ 0, kDNSServiceInterfaceIndexAny,
                          kDNSServiceProtocol_IPv6 | kDNSServiceProtocol_IPv4, fullHostName.c_str(),
                          HandleResolveResult, this);
}

void PublisherMDnsSd::HostSubscription::HandleResolveResult(DNSServiceRef          aServiceRef,
                                                            DNSServiceFlags        aFlags,
                                                            uint32_t               aInterfaceIndex,
                                                            DNSServiceErrorType    aErrorCode,
                                                            const char            *aHostName,
                                                            const struct sockaddr *aAddress,
                                                            uint32_t               aTtl,
                                                            void                  *aContext)
{
    static_cast<HostSubscription *>(aContext)->HandleResolveResult(aServiceRef, aFlags, aInterfaceIndex, aErrorCode,
                                                                   aHostName, aAddress, aTtl);
}

void PublisherMDnsSd::HostSubscription::HandleResolveResult(DNSServiceRef          aServiceRef,
                                                            DNSServiceFlags        aFlags,
                                                            uint32_t               aInterfaceIndex,
                                                            DNSServiceErrorType    aErrorCode,
                                                            const char            *aHostName,
                                                            const struct sockaddr *aAddress,
                                                            uint32_t               aTtl)
{
    OTBR_UNUSED_VARIABLE(aServiceRef);
    OTBR_UNUSED_VARIABLE(aInterfaceIndex);

    Ip6Address address;

    otbrLog(aErrorCode == kDNSServiceErr_NoError ? OTBR_LOG_INFO : OTBR_LOG_WARNING, OTBR_LOG_TAG,
            "DNSServiceGetAddrInfo reply: flags=%" PRIu32 ", host=%s, sa_family=%u, error=%" PRId32, aFlags, aHostName,
            static_cast<unsigned int>(aAddress->sa_family), aErrorCode);

    VerifyOrExit(aErrorCode == kDNSServiceErr_NoError);
    VerifyOrExit((aFlags & kDNSServiceFlagsAdd) && aAddress->sa_family == AF_INET6);

    address.CopyFrom(*reinterpret_cast<const struct sockaddr_in6 *>(aAddress));
    VerifyOrExit(!address.IsLinkLocal(),
                 otbrLogDebug("DNSServiceGetAddrInfo ignore link-local address %s", address.ToString().c_str()));

    mHostInfo.mHostName = aHostName;
    mHostInfo.mAddresses.push_back(address);
    mHostInfo.mTtl = aTtl;

    otbrLogInfo("DNSServiceGetAddrInfo reply: address=%s, ttl=%" PRIu32, address.ToString().c_str(), aTtl);

    // NOTE: This `HostSubscription` object may be freed in `OnHostResolved`.
    mMDnsSd->OnHostResolved(mHostName, mHostInfo);

exit:
    if (aErrorCode != kDNSServiceErr_NoError)
    {
        mMDnsSd->OnHostResolveFailed(aHostName, aErrorCode);
    }
}

} // namespace Mdns

} // namespace otbr
