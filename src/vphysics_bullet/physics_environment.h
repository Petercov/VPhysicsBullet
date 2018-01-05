// Copyright Valve Corporation, All rights reserved.
// Bullet integration by Triang3l, derivative work, in public domain if detached from Valve's work.

#ifndef PHYSICS_ENVIRONMENT_H
#define PHYSICS_ENVIRONMENT_H

#include "physics_internal.h"
#include "vphysics/performance.h"
#include "tier1/utlrbtree.h"
#include "tier1/utlvector.h"

class CPhysicsEnvironment : public IPhysicsEnvironment {
public:
	CPhysicsEnvironment();
	virtual ~CPhysicsEnvironment();

	// IPhysicsEnvironment methods.

	virtual void SetGravity(const Vector &gravityVector);
	virtual void GetGravity(Vector *pGravityVector) const;

	virtual void SetCollisionEventHandler(IPhysicsCollisionEvent *pCollisionEvents);

	virtual void GetPerformanceSettings(physics_performanceparams_t *pOutput) const;
	virtual void SetPerformanceSettings(const physics_performanceparams_t *pSettings);

	// Internal methods.

	FORCEINLINE btDiscreteDynamicsWorld *GetDynamicsWorld() {
		return m_DynamicsWorld;
	}
	FORCEINLINE const btDiscreteDynamicsWorld *GetDynamicsWorld() const {
		return m_DynamicsWorld;
	}

	void NotifyObjectRemoving(IPhysicsObject *object);

	void NotifyTriggerRemoved(IPhysicsObject *trigger);

	FORCEINLINE btScalar GetMaxSpeed() const {
		return HL2BULLET(m_PerformanceSettings.maxVelocity);
	}
	FORCEINLINE btScalar GetMaxAngularSpeed() const {
		return DEG2RAD(m_PerformanceSettings.maxAngularVelocity);
	}

private:
	btDefaultCollisionConfiguration *m_CollisionConfiguration;
	btCollisionDispatcher *m_Dispatcher;
	btBroadphaseInterface *m_Broadphase;
	btSequentialImpulseConstraintSolver *m_Solver;
	btDiscreteDynamicsWorld *m_DynamicsWorld;

	CUtlVector<CPhysCollide *> m_SphereCache;

	CUtlVector<IPhysicsObject *> m_NonStaticObjects;

	IPhysicsCollisionEvent *m_CollisionEvents;

	struct TriggerTouch_t {
		IPhysicsObject *m_Trigger;
		IPhysicsObject *m_Object;
		// Temporary flag indicating there's still a touch between this pair.
		// Touches with this flag being false after checking all contacts are removed.
		bool m_TouchingThisTick;

		TriggerTouch_t(IPhysicsObject *trigger, IPhysicsObject *object) :
				m_Trigger(trigger), m_Object(object), m_TouchingThisTick(true) {}
	};
	static bool TriggerTouchLessFunc(const TriggerTouch_t &lhs, const TriggerTouch_t &rhs) {
		if (lhs.m_Trigger != rhs.m_Trigger) {
			return (lhs.m_Trigger < rhs.m_Trigger);
		}
		return (lhs.m_Object < rhs.m_Object);
	}
	CUtlRBTree<TriggerTouch_t> m_TriggerTouches;
	void CheckTriggerTouches();

	physics_performanceparams_t m_PerformanceSettings;

	static void PreTickCallback(btDynamicsWorld *world, btScalar timeStep);
	static void TickCallback(btDynamicsWorld *world, btScalar timeStep);
};

#endif
