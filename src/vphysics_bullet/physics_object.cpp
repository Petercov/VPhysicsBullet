// Copyright Valve Corporation, All rights reserved.
// Bullet integration by Triang3l, derivative work, in public domain if detached from Valve's work.

#include "physics_object.h"
#include "physics_collision.h"
#include "physics_environment.h"
#include "bspflags.h"
#include "tier0/dbg.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

CPhysicsObject::CPhysicsObject(IPhysicsEnvironment *environment,
		btCollisionShape *collisionShape, int materialIndex,
		const Vector &position, const QAngle &angles,
		objectparams_t *pParams, bool isStatic) :
		m_Environment(environment),
		m_CollideObjectNext(this), m_CollideObjectPrevious(this),
		m_MassCenterOverride(0.0f, 0.0f, 0.0f), m_MassCenterOverrideShape(nullptr),
		m_Mass((!isStatic && !collisionShape->isNonMoving()) ? pParams->mass : 0.0f),
		m_Inertia(pParams->inertia, pParams->inertia, pParams->inertia),
		m_Damping(pParams->damping), m_RotDamping(pParams->rotdamping),
		m_GameData(nullptr), m_GameFlags(0), m_GameIndex(0),
		m_Callbacks(CALLBACK_GLOBAL_COLLISION | CALLBACK_GLOBAL_FRICTION |
				CALLBACK_FLUID_TOUCH | CALLBACK_GLOBAL_TOUCH |
				CALLBACK_GLOBAL_COLLIDE_STATIC | CALLBACK_DO_FLUID_SIMULATION),
		m_ContentsMask(CONTENTS_SOLID),
		m_LinearVelocityChange(0.0f, 0.0f, 0.0f),
		m_LocalAngularVelocityChange(0.0f, 0.0f, 0.0f) {
	VectorAbs(m_Inertia, m_Inertia);
	btVector3 inertia;
	ConvertDirectionToBullet(m_Inertia, inertia);

	btRigidBody::btRigidBodyConstructionInfo constructionInfo(
			m_Mass, nullptr, collisionShape, inertia.absolute());

	btVector3 massCenter = g_pPhysCollision->CollideGetBulletMassCenter(collisionShape);
	const Vector *massCenterOverride = pParams->massCenterOverride;
	if (massCenterOverride != nullptr && *massCenterOverride != vec3_origin) {
		ConvertPositionToBullet(*massCenterOverride, m_MassCenterOverride);
		BEGIN_BULLET_ALLOCATION();
		m_MassCenterOverrideShape = new btCompoundShape(false, 1);
		m_MassCenterOverrideShape->addChildShape(btTransform(btMatrix3x3::getIdentity(),
				massCenter - m_MassCenterOverride), collisionShape);
		END_BULLET_ALLOCATION();
		constructionInfo.m_collisionShape = m_MassCenterOverrideShape;
		massCenter = m_MassCenterOverride;
	}

	matrix3x4_t startMatrix;
	AngleMatrix(angles, position, startMatrix);
	btTransform &startWorldTransform = constructionInfo.m_startWorldTransform;
	ConvertMatrixToBullet(startMatrix, startWorldTransform);
	startWorldTransform.getOrigin() += startWorldTransform.getBasis() * massCenter;

	BEGIN_BULLET_ALLOCATION();
	m_RigidBody = new btRigidBody(constructionInfo);
	END_BULLET_ALLOCATION();
	m_RigidBody->setUserPointer(this);

	AddReferenceToCollide();
}

CPhysicsObject::~CPhysicsObject() {
	// Prevent callbacks to the game code and unlink from this object.
	m_Callbacks = 0;
	m_GameData = nullptr;

	// TODO: Delete or add to the deletion queue.
}

btCollisionShape *CPhysicsObject::GetCollisionShape() const {
	if (m_MassCenterOverrideShape != nullptr) {
		return m_MassCenterOverrideShape->getChildShape(0);
	}
	return m_RigidBody->getCollisionShape();
}

/*******************
 * Mass and inertia
 *******************/

bool CPhysicsObject::IsStatic() const {
	return m_RigidBody->isStaticObject();
}

void CPhysicsObject::SetMass(float mass) {
	Assert(mass > 0.0f);
	if (IsStatic()) {
		return;
	}
	m_Mass = mass;
	btVector3 bulletInertia;
	ConvertDirectionToBullet(m_Inertia, bulletInertia);
	m_RigidBody->setMassProps(mass, bulletInertia.absolute());
}

float CPhysicsObject::GetMass() const {
	return m_Mass;
}

float CPhysicsObject::GetInvMass() const {
	return m_RigidBody->getInvMass();
}

Vector CPhysicsObject::GetInertia() const {
	return m_Inertia;
}

Vector CPhysicsObject::GetInvInertia() const {
	Vector inertia;
	ConvertDirectionToHL(m_RigidBody->getInvInertiaDiagLocal(), inertia);
	VectorAbs(inertia, inertia);
	return inertia;
}

void CPhysicsObject::SetInertia(const Vector &inertia) {
	m_Inertia = inertia;
	VectorAbs(m_Inertia, m_Inertia);
	btVector3 bulletInertia;
	ConvertDirectionToBullet(inertia, bulletInertia);
	m_RigidBody->setMassProps(m_Mass, bulletInertia.absolute());
	m_RigidBody->updateInertiaTensor();
}

bool CPhysicsObject::IsMotionEnabled() const {
	return !m_RigidBody->getAngularFactor().isZero();
}

bool CPhysicsObject::IsMoveable() const {
	return !IsStatic() && IsMotionEnabled();
}

void CPhysicsObject::EnableMotion(bool enable) {
	if (IsMotionEnabled() == enable) {
		return;
	}

	btVector3 zero(0.0f, 0.0f, 0.0f);

	// IVP clears velocity even if unpinning.
	m_RigidBody->clearForces();
	m_RigidBody->setLinearVelocity(zero);
	m_RigidBody->setAngularVelocity(zero);
	m_LinearVelocityChange.setZero();
	m_LocalAngularVelocityChange.setZero();

	if (enable) {
		btVector3 one(1.0f, 1.0f, 1.0f);
		m_RigidBody->setLinearFactor(one);
		m_RigidBody->setAngularFactor(one);
	} else {
		m_RigidBody->setLinearFactor(zero);
		m_RigidBody->setAngularFactor(zero);
	}
}

/*******************
 * Activation state
 *******************/

bool CPhysicsObject::IsAsleep() const {
	return !m_RigidBody->isActive();
}

void CPhysicsObject::Wake() {
	if (!IsStatic() && m_RigidBody->getActivationState() != DISABLE_DEACTIVATION) {
		// Forcing because it may be used for external forces without contacts.
		// Also waking up from DISABLE_SIMULATION, which is not possible with setActivationState.
		m_RigidBody->forceActivationState(ACTIVE_TAG);
		m_RigidBody->setDeactivationTime(0.0f);
	}
}

void CPhysicsObject::Sleep() {
	if (!IsStatic() && m_RigidBody->getActivationState() != DISABLE_DEACTIVATION) {
		m_RigidBody->setActivationState(DISABLE_SIMULATION);
	}
}

/**********************
 * Gravity and damping
 **********************/

bool CPhysicsObject::IsGravityEnabled() const {
	return !IsStatic() && !(m_RigidBody->getFlags() & BT_DISABLE_WORLD_GRAVITY);
}

void CPhysicsObject::EnableGravity(bool enable) {
	if (IsStatic()) {
		return;
	}

	int flags = m_RigidBody->getFlags();
	if (enable == !(flags & BT_DISABLE_WORLD_GRAVITY)) {
		return;
	}

	if (enable) {
		const CPhysicsEnvironment *environment = static_cast<CPhysicsEnvironment *>(m_Environment);
		m_RigidBody->setGravity(environment->GetDynamicsWorld()->getGravity());
		m_RigidBody->setFlags(flags & ~BT_DISABLE_WORLD_GRAVITY);
	} else {
		m_RigidBody->setGravity(btVector3(0.0f, 0.0f, 0.0f));
		m_RigidBody->setFlags(flags | BT_DISABLE_WORLD_GRAVITY);
	}
}

void CPhysicsObject::SetDamping(const float *speed, const float *rot) {
	if (speed != nullptr) {
		m_Damping = *speed;
	}
	if (rot != nullptr) {
		m_RotDamping = *rot;
	}
}

void CPhysicsObject::GetDamping(float *speed, float *rot) const {
	if (speed != nullptr) {
		*speed = m_Damping;
	}
	if (rot != nullptr) {
		*rot = m_RotDamping;
	}
}

void CPhysicsObject::ApplyDamping(float timeStep) {
	if (m_RigidBody->isStaticOrKinematicObject() || !IsGravityEnabled()) {
		return;
	}

	const btVector3 &linearVelocity = m_RigidBody->getLinearVelocity();
	const btVector3 &angularVelocity = m_RigidBody->getAngularVelocity();

	btScalar damping = m_Damping, rotDamping = m_RotDamping;
	if (linearVelocity.length2() < 0.01f && angularVelocity.length2() < 0.01f) {
		damping += 0.1f;
		rotDamping += 0.1f;
	}
	damping *= timeStep;
	rotDamping *= timeStep;

	if (damping < 0.25f) {
		damping = btScalar(1.0f) - damping;
	} else {
		damping = btExp(-damping);
	}
	m_RigidBody->setLinearVelocity(linearVelocity * damping);

	if (rotDamping < 0.4f) {
		rotDamping = btScalar(1.0f) - rotDamping;
	} else {
		rotDamping = btExp(-rotDamping);
	}
	m_RigidBody->setAngularVelocity(angularVelocity * rotDamping);
}

/************
 * Game data
 ************/

void CPhysicsObject::SetGameData(void *pGameData) {
	m_GameData = pGameData;
}

void *CPhysicsObject::GetGameData() const {
	return m_GameData;
}

void CPhysicsObject::SetGameFlags(unsigned short userFlags) {
	m_GameFlags = userFlags;
}

unsigned short CPhysicsObject::GetGameFlags() const {
	return m_GameFlags;
}

void CPhysicsObject::SetGameIndex(unsigned short gameIndex) {
	m_GameIndex = gameIndex;
}

unsigned short CPhysicsObject::GetGameIndex() const {
	return m_GameIndex;
}

void CPhysicsObject::SetCallbackFlags(unsigned short callbackflags) {
	m_Callbacks = callbackflags;
}

unsigned short CPhysicsObject::GetCallbackFlags() const {
	return m_Callbacks;
}

unsigned int CPhysicsObject::GetContents() const {
	return m_ContentsMask;
}

void CPhysicsObject::SetContents(unsigned int contents) {
	m_ContentsMask = contents;
}

/**********************
 * Position and forces
 **********************/

const btVector3 &CPhysicsObject::GetBulletMassCenter() const {
	if (m_MassCenterOverrideShape != nullptr) {
		return m_MassCenterOverride;
	}
	return g_pPhysCollision->CollideGetBulletMassCenter(m_RigidBody->getCollisionShape());
}

Vector CPhysicsObject::GetMassCenterLocalSpace() const {
	Vector massCenter;
	ConvertPositionToHL(GetBulletMassCenter(), massCenter);
	return massCenter;
}

void CPhysicsObject::NotifyMassCenterChanged(const btVector3 &oldMassCenter) {
	const btVector3 &newMassCenter = g_pPhysCollision->CollideGetBulletMassCenter(GetCollisionShape());
	if (m_MassCenterOverrideShape != nullptr) {
		btTransform childTransform = m_MassCenterOverrideShape->getChildTransform(0);
		childTransform.setOrigin(newMassCenter - m_MassCenterOverride);
		m_MassCenterOverrideShape->updateChildTransform(0, childTransform);
	} else {
		// Updates the same properties as setCenterOfMassTransform.
		btVector3 offset = newMassCenter - oldMassCenter;
		btTransform worldTransform = m_RigidBody->getWorldTransform();
		btVector3 worldOffset = worldTransform.getBasis() * offset;
		worldTransform.getOrigin() += worldOffset;
		m_RigidBody->setWorldTransform(worldTransform);
		btTransform interpolationWorldTransform = m_RigidBody->getInterpolationWorldTransform();
		interpolationWorldTransform.getOrigin() += worldOffset;
		m_RigidBody->setInterpolationWorldTransform(interpolationWorldTransform);
	}
}

void CPhysicsObject::SetPosition(const Vector &worldPosition, const QAngle &angles, bool isTeleport) {
	// TODO: Update the shadow.
	matrix3x4_t matrix;
	AngleMatrix(angles, worldPosition, matrix);
	btTransform transform;
	ConvertMatrixToBullet(matrix, transform);
	transform.getOrigin() += transform.getBasis() * GetBulletMassCenter();
	m_RigidBody->proceedToTransform(transform);
}

void CPhysicsObject::SetPositionMatrix(const matrix3x4_t &matrix, bool isTeleport) {
	// TODO: Update the shadow.
	btTransform transform;
	ConvertMatrixToBullet(matrix, transform);
	transform.getOrigin() += transform.getBasis() * GetBulletMassCenter();
	m_RigidBody->proceedToTransform(transform);
}

void CPhysicsObject::GetPosition(Vector *worldPosition, QAngle *angles) const {
	const btTransform &transform = m_RigidBody->getWorldTransform();
	const btMatrix3x3 &basis = transform.getBasis();
	if (worldPosition != nullptr) {
		ConvertPositionToHL(transform.getOrigin() - basis * GetBulletMassCenter(), *worldPosition);
	}
	if (angles != nullptr) {
		ConvertRotationToHL(basis, *angles);
	}
}

void CPhysicsObject::GetPositionMatrix(matrix3x4_t *positionMatrix) const {
	const btTransform &transform = m_RigidBody->getWorldTransform();
	const btMatrix3x3 &basis = transform.getBasis();
	btVector3 origin = transform.getOrigin() - basis * GetBulletMassCenter();
	ConvertMatrixToHL(basis, origin, *positionMatrix);
}

void CPhysicsObject::LocalToWorld(Vector *worldPosition, const Vector &localPosition) const {
	matrix3x4_t matrix;
	GetPositionMatrix(&matrix);
	// Copy in case src == dest.
	VectorTransform(Vector(localPosition), matrix, *worldPosition);
}

void CPhysicsObject::WorldToLocal(Vector *localPosition, const Vector &worldPosition) const {
	matrix3x4_t matrix;
	GetPositionMatrix(&matrix);
	// Copy in case src == dest.
	VectorITransform(Vector(worldPosition), matrix, *localPosition);
}

void CPhysicsObject::LocalToWorldVector(Vector *worldVector, const Vector &localVector) const {
	matrix3x4_t matrix;
	GetPositionMatrix(&matrix);
	// Copy in case src == dest.
	VectorRotate(Vector(localVector), matrix, *worldVector);
}

void CPhysicsObject::WorldToLocalVector(Vector *localVector, const Vector &worldVector) const {
	matrix3x4_t matrix;
	GetPositionMatrix(&matrix);
	// Copy in case src == dest.
	VectorIRotate(Vector(worldVector), matrix, *localVector);
}

void CPhysicsObject::ApplyForcesAndSpeedLimit() {
	if (IsMoveable() && !IsAsleep()) {
		const CPhysicsEnvironment *environment = static_cast<const CPhysicsEnvironment *>(m_Environment);

		btVector3 linearVelocity = m_RigidBody->getLinearVelocity();
		linearVelocity += m_LinearVelocityChange * m_RigidBody->getLinearFactor();
		btScalar maxSpeed = environment->GetMaxSpeed();
		btClamp(linearVelocity[0], -maxSpeed, maxSpeed);
		btClamp(linearVelocity[1], -maxSpeed, maxSpeed);
		btClamp(linearVelocity[2], -maxSpeed, maxSpeed);
		m_RigidBody->setLinearVelocity(linearVelocity);

		btVector3 angularVelocity = m_RigidBody->getAngularVelocity();
		angularVelocity += (m_RigidBody->getWorldTransform().getBasis() * m_LocalAngularVelocityChange) *
				m_RigidBody->getAngularFactor();
		btScalar maxAngularSpeed = environment->GetMaxAngularSpeed();
		btClamp(angularVelocity[0], -maxAngularSpeed, maxAngularSpeed);
		btClamp(angularVelocity[1], -maxAngularSpeed, maxAngularSpeed);
		btClamp(angularVelocity[2], -maxAngularSpeed, maxAngularSpeed);
		m_RigidBody->setAngularVelocity(angularVelocity);
	}

	m_LinearVelocityChange.setZero();
	m_LocalAngularVelocityChange.setZero();
}

void CPhysicsObject::SetVelocity(const Vector *velocity, const AngularImpulse *angularVelocity) {
	if (!IsMoveable()) {
		return;
	}
	Wake();
	btVector3 zero(0.0f, 0.0f, 0.0f);
	if (velocity != nullptr) {
		ConvertPositionToBullet(*velocity, m_LinearVelocityChange);
		m_RigidBody->setLinearVelocity(zero);
	}
	if (angularVelocity != nullptr) {
		ConvertAngularImpulseToBullet(*angularVelocity, m_LocalAngularVelocityChange);
		m_RigidBody->setAngularVelocity(zero);
	}
}

void CPhysicsObject::GetVelocity(Vector *velocity, AngularImpulse *angularVelocity) const {
	if (velocity != nullptr) {
		ConvertPositionToHL(m_RigidBody->getLinearVelocity() + m_LinearVelocityChange, *velocity);
	}
	if (angularVelocity != nullptr) {
		AngularImpulse worldAngularVelocity;
		ConvertAngularImpulseToHL(m_RigidBody->getAngularVelocity() +
				(m_RigidBody->getWorldTransform().getBasis() * m_LocalAngularVelocityChange),
				worldAngularVelocity);
		WorldToLocalVector(angularVelocity, worldAngularVelocity);
	}
}

void CPhysicsObject::AddVelocity(const Vector *velocity, const AngularImpulse *angularVelocity) {
	if (!IsMoveable()) {
		return;
	}
	Wake();
	if (velocity != nullptr) {
		btVector3 bulletVelocity;
		ConvertPositionToBullet(*velocity, bulletVelocity);
		m_LinearVelocityChange += bulletVelocity;
	}
	if (angularVelocity != nullptr) {
		btVector3 bulletAngularVelocity;
		ConvertAngularImpulseToBullet(*angularVelocity, bulletAngularVelocity);
		m_LocalAngularVelocityChange += bulletAngularVelocity;
	}
}

void CPhysicsObject::ApplyForceCenter(const Vector &forceVector) {
	if (!IsMoveable()) {
		return;
	}
	btVector3 bulletForce;
	ConvertForceImpulseToBullet(forceVector, bulletForce);
	m_LinearVelocityChange += bulletForce * m_RigidBody->getInvMass();
	Wake();
}

void CPhysicsObject::ApplyForceOffset(const Vector &forceVector, const Vector &worldPosition) {
	if (!IsMoveable()) {
		return;
	}

	btVector3 bulletWorldForce;
	ConvertForceImpulseToBullet(forceVector, bulletWorldForce);
	m_LinearVelocityChange += bulletWorldForce * m_RigidBody->getInvMass();

	Vector localForce;
	WorldToLocalVector(&localForce, forceVector);
	btVector3 bulletLocalForce;
	ConvertForceImpulseToBullet(localForce, bulletLocalForce);
	Vector localPosition;
	WorldToLocal(&localPosition, worldPosition);
	btVector3 bulletLocalPosition;
	ConvertPositionToBullet(localPosition, bulletLocalPosition);
	m_LocalAngularVelocityChange += bulletLocalPosition.cross(bulletLocalForce) *
			m_RigidBody->getInvInertiaDiagLocal();

	Wake();
}

void CPhysicsObject::ApplyTorqueCenter(const AngularImpulse &torque) {
	if (!IsMoveable()) {
		return;
	}
	AngularImpulse localTorque;
	WorldToLocalVector(&localTorque, torque);
	btVector3 bulletLocalTorque;
	ConvertAngularImpulseToBullet(localTorque, bulletLocalTorque);
	m_LocalAngularVelocityChange += bulletLocalTorque *
			m_RigidBody->getInvInertiaDiagLocal();
	Wake();
}

/***************************************
 * Collide object reference linked list
 ***************************************/

void CPhysicsObject::AddReferenceToCollide() {
	btCollisionShape *shape = GetCollisionShape();
	void *nextPointer = shape->getUserPointer();
	shape->setUserPointer(static_cast<IPhysicsObject *>(this));
	if (nextPointer != nullptr) {
		m_CollideObjectNext = static_cast<CPhysicsObject *>(
				reinterpret_cast<IPhysicsObject *>(nextPointer));
		m_CollideObjectPrevious = m_CollideObjectNext->m_CollideObjectPrevious;
		m_CollideObjectNext->m_CollideObjectPrevious = this;
		m_CollideObjectPrevious->m_CollideObjectNext = this;
	} else {
		m_CollideObjectNext = m_CollideObjectPrevious = this;
	}
}

void CPhysicsObject::RemoveReferenceFromCollide() {
	btCollisionShape *shape = GetCollisionShape();
	void *nextPointer = shape->getUserPointer();
	if (nextPointer != nullptr && reinterpret_cast<IPhysicsObject *>(nextPointer) == this) {
		shape->setUserPointer(m_CollideObjectNext != this ?
				static_cast<IPhysicsObject *>(m_CollideObjectNext) : nullptr);
	}
	m_CollideObjectNext->m_CollideObjectPrevious = m_CollideObjectPrevious;
	m_CollideObjectPrevious->m_CollideObjectNext = m_CollideObjectNext;
}
