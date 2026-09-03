#pragma once
#include "ResourceServerRequest.h"
#include "EngineTools/ThirdParty/subprocess/subprocess.h"
#include "EngineTools/Resource/ResourceCompilerNetworkMessages.h"
#include "Base/Resource/ResourceProviders/ResourceNetworkMessages.h"
#include "Base/Time/Time.h"
#include "Base/Time/Timers.h"
#include "Base/Types/UUID.h"

//-------------------------------------------------------------------------

namespace EE::Resource
{
    struct ResourceServerContext;
    struct Request;

    //-------------------------------------------------------------------------

    struct WorkerTask : public RequestBucket
    {
        friend class ResourceServerWorker;

    public:

        enum class State : uint8_t
        {
            Pending,
            Requested,
            Complete
        };

        WorkerTask() = default;

        WorkerTask( Request const* pRequest ) : RequestBucket( pRequest ) {}

        void AddRequest( RequestBucket const* pRequestBucket );

    public:

        UUID                                                m_ID = UUID::GenerateID();
        String                                              m_log;
        Timer<PlatformClock>                                m_timeSinceLastHeartbeat;
        int8_t                                              m_retryAttempts = 0;
        State                                               m_state = State::Pending;
    };

    //-------------------------------------------------------------------------

    class ResourceServerWorker
    {
        // How long between heartbeats should we wait before re-sending the request?
        constexpr static float const s_heartbeatTimeoutSeconds = 5.0f;

        // How many times are we allowed to retry a task?
        constexpr static uint8_t const s_maxRetryAttempts = 5;

        struct NetworkResponseBucket
        {
            void Reset();
            void AddUpdateResponse( ResourceID const& ID, String const& filePath, String const& log = String() );
            void AddRequestResponse( ResourceID const& ID, String const& filePath, String const& log = String() );
            void AddRequestHeartbeat( ResourceID const& ID );

        public:

            uint64_t                         m_clientID;
            TVector<NetworkResourceResponse> m_updateResponses;
            TVector<NetworkResourceResponse> m_requestResponses;
            TVector<NetworkResourceResponse> m_requestHeartbeats;
        };

    public:

        ResourceServerWorker( ResourceServerContext& context, uint64_t workerID );
        ~ResourceServerWorker();

        void Update();

        inline bool IsBusy() const { return !m_tasks.empty(); }
        inline int32_t GetNumTasks() const { return (int32_t) m_tasks.size(); }
        void CreateTask( RequestBucket const* pRequestBucket );
        WorkerTask* TryFindTaskMatchingRequest( RequestBucket const* pRequestBucket ) const;

        void HandleHeartbeatMessage( NetworkResourceCompilerRequest&& msg );
        void HandleCompletionMessage( NetworkResourceCompilerResponse&& msg );

    private:

        void CheckIfProcessStillRunning();
        void ProcessPendingTasks();
        void ProcessTimedOutTasks();
        void ProcessCompletionResults();
        void GenerateHeartbeats();

        void FailTask( WorkerTask* pTask, String const& reason );

    public:

        int64_t const                                       m_workerID;
        ResourceServerContext&                              m_context;
        subprocess_s                                        m_process;
        String                                              m_processOutput;

        TVector<WorkerTask*>                                m_tasks;
        TVector<NetworkResourceCompilerResponse::Result>    m_completionResults;

        uint64_t                                            m_networkClientID = 0;
        TInlineVector<NetworkResponseBucket, 20>            m_responseBuckets;

        Log                                                 m_log;
    };
}
