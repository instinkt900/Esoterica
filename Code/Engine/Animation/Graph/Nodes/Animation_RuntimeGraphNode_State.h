#pragma once

#include "Animation_RuntimeGraphNode_Passthrough.h"

//-------------------------------------------------------------------------

namespace EE::Animation
{
    class EE_ENGINE_API StateNode final : public PoseNode
    {
        friend class TransitionNode;
        friend class StateMachineNode;

    public:

        enum class TransitionState : uint8_t
        {
            None,
            TransitioningIn,
            TransitioningOut,
        };

        // This is a helper to set a flag in the parent state machine
        struct TransitionFlag
        {
            TransitionFlag() = default;

            TransitionFlag( uint64_t* pFlags, uint8_t idx ) : m_pFlags ( pFlags )
            {
                EE_ASSERT( idx < 64 );
                m_flagMask = uint64_t( 1 ) << idx;
            }

            EE_FORCE_INLINE void Set( bool isSet )
            {
                if ( isSet )
                {
                    ( *m_pFlags ) |= m_flagMask;
                }
                else
                {
                    ( *m_pFlags ) &= ~m_flagMask;
                }
            }

        private:

            uint64_t*   m_pFlags = nullptr;
            uint64_t    m_flagMask = 0;
        };

        //-------------------------------------------------------------------------

        struct TimedEvent
        {
            EE_SERIALIZE( m_ID, m_timeValue, m_comparisonOperator );

            enum class Comparison : uint8_t
            {
                EE_REFLECT_ENUM

                LessThanEqual,
                GreaterThanEqual,
            };

        public:

            TimedEvent() = default;

            TimedEvent( StringID ID, Seconds value, Comparison op )
                : m_ID ( ID )
                , m_timeValue( value )
                , m_comparisonOperator( op )
            {}

        public:

            StringID                                    m_ID;
            Seconds                                     m_timeValue;
            Comparison                                  m_comparisonOperator;
        };

        //-------------------------------------------------------------------------

        struct EE_ENGINE_API Definition : public PoseNode::Definition
        {
            EE_REFLECT_TYPE( Definition );
            EE_SERIALIZE_GRAPHNODEDEFINITION( PoseNode::Definition, m_childNodeIdx, m_entryEvents, m_fullyInStateEvents, m_exitEvents, m_timedRemainingEvents, m_timedElapsedEvents, m_layerBoneMaskNodeIdx, m_layerRootMotionWeightNodeIdx,  m_layerWeightNodeIdx, m_isOffState );

        public:

            virtual void InstantiateNode( InstantiationContext const& context, InstantiationOptions options ) const override;

        public:

            int16_t                                     m_childNodeIdx = InvalidIndex;
            TInlineVector<StringID, 3>                  m_entryEvents;
            TInlineVector<StringID, 3>                  m_fullyInStateEvents;
            TInlineVector<StringID, 3>                  m_exitEvents;
            TInlineVector<TimedEvent, 1>                m_timedRemainingEvents;
            TInlineVector<TimedEvent, 1>                m_timedElapsedEvents;
            int16_t                                     m_layerWeightNodeIdx = InvalidIndex;
            int16_t                                     m_layerRootMotionWeightNodeIdx = InvalidIndex;
            int16_t                                     m_layerBoneMaskNodeIdx = InvalidIndex;
            bool                                        m_isOffState = false;
            bool                                        m_useActualElapsedTimeInStateForTimedEvents = false;
        };

    public:

        virtual bool IsValid() const override { return PoseNode::IsValid() && m_pChildNode != nullptr && m_pChildNode->IsValid(); }
        virtual SyncTrack const& GetSyncTrack() const override;

        virtual GraphPoseNodeResult Update( GraphContext& context, SyncTrackTimeRange const* pUpdateRange ) override;

        // State info
        inline SampledEventRange GetSampledEventRange() const { return m_sampledEventRange; }
        inline bool IsOffState() const { return GetDefinition<StateNode>()->m_isOffState; }

        // Transitions
        inline bool IsTransitioning() const { return m_transitionState != TransitionState::None; }
        inline bool IsTransitioningIn() const { return m_transitionState == TransitionState::TransitioningIn; }
        inline bool IsTransitioningOut() const { return m_transitionState == TransitionState::TransitioningOut; }

        inline void SetTransitionState( TransitionState newState )
        {
            m_transitionState = newState;
            m_transitionFlag.Set( IsTransitioning() );
        }

    private:

        virtual void InitializeInternal( GraphContext& context, SyncTrackTime const& initialTime ) override;
        virtual void ShutdownInternal( GraphContext& context ) override;

        // Starts a transition into this state
        void StartTransitionIn( GraphContext& context );

        // Starts a transition out of this state, this call may change the sampled event range so we return the new range
        SampledEventRange StartTransitionOut( GraphContext& context, bool isZeroDurationTransition );

        // Sample all the state events for this given update
        void SampleStateEvents( GraphContext& context );

        // Update the layer weights for this state
        void UpdateLayerContext( GraphContext& context );

        virtual void RecordGraphState( RecordedGraphState& outState ) override;
        virtual bool RestoreGraphState( RecordedGraphState const& inState ) override;

    private:

        PoseNode*                                       m_pChildNode = nullptr;
        SampledEventRange                               m_sampledEventRange;
        BoneMaskValueNode*                              m_pBoneMaskNode = nullptr;
        FloatValueNode*                                 m_pLayerWeightNode = nullptr;
        FloatValueNode*                                 m_pLayerRootMotionWeightNode = nullptr;
        TransitionState                                 m_transitionState = TransitionState::None;
        TransitionFlag                                  m_transitionFlag;
        Seconds                                         m_elapsedTime = 0.0f;
        bool                                            m_isFirstStateUpdate = false;
    };
}