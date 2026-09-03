#pragma once

#include "ResourceNetworkMessages.h"
#include "Base/Resource/ResourceProvider.h"
#include "Base/Resource/ResourceRequest.h"
#include "Base/Time/Timers.h"
#include "Base/Network/Clients/NetworkClient_WebSockets.h"
#include "Base/Threading/TaskSystem.h"

//-------------------------------------------------------------------------

#if EE_DEVELOPMENT_TOOLS
namespace EE::Resource
{
    class ResourceSettings;

    //-------------------------------------------------------------------------

    class EE_BASE_API NetworkResourceProvider final : public ResourceProvider
    {
        struct SentRequest
        {
            enum class State : uint8_t
            {
                Requested,  // Requested and sent but we havent received a heartbeat back
                Acknowledged // Requested and sent and we've received a heartbeat back
            };

            inline bool operator==( ResourceRequest* const& pRequest ) const
            {
                return m_pRequest == pRequest;
            }

            inline bool operator==( ResourceID const& resourceID ) const
            {
                return m_pRequest->GetResourceID() == resourceID;
            }

            inline Seconds GetTimeSinceLastHeartbeat() const { return m_timeSinceLastHeartbeat.GetElapsedTimeSeconds(); }

        public:

            ResourceRequest*                                m_pRequest = nullptr;
            Timer<PlatformClock>                            m_timeSinceLastHeartbeat;
            int8_t                                          m_retryCount = 0;
            State                                           m_state = State::Requested;
        };

        friend class ResourceDebugView;

    public:

        NetworkResourceProvider( ResourceSettings const& settings, TaskSystem& taskSystem );
        ~NetworkResourceProvider();

        virtual bool IsReady() const override final;
        virtual bool IsConnecting() const override final;

    private:

        virtual bool Initialize() override final;
        virtual void Shutdown() override final;
        virtual void Update() override final;

        virtual void RequestRawResource( ResourceRequest* pRequest ) override;
        virtual void CancelRequest( ResourceRequest* pRequest ) override;

        virtual TVector<ResourceID> const& GetExternallyUpdatedResources() const override { return m_externallyUpdatedResources; }

        void DeserializeReceivedMessages();

    private:

        Network::Client_WS                                  m_networkClient;
        TaskSystem&                                         m_taskSystem;
        Threading::Mutex                                    m_requestModificationMutex;

        // We need a separate queue cause deserialization of resource ID is pretty expensive and we dont want to do this on the main thread
        Threading::TLockFreeQueue<Network::Message*>        m_unserializedMessages;
        AsyncTask                                           m_deserializationTask;
        TVector<NetworkResourceResponse::Result>            m_deserializedServerResults;
        TVector<NetworkResourceResponse::Result>            m_deserializedServerHeartbeats;
        TVector<ResourceID>                                 m_deserializedExternalResourceUpdates;

        // Unprocessed results
        TVector<NetworkResourceResponse::Result>            m_serverResults;
        TVector<NetworkResourceResponse::Result>            m_serverHeartbeats;
        TVector<ResourceID>                                 m_externallyUpdatedResources;

        // Requests
        TVector<ResourceRequest*>                           m_pendingRequests; // Requests we need to still send
        TVector<SentRequest>                                m_sentRequests; // Request that were sent but we're still waiting for a response
    };
}
#endif