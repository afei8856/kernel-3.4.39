/*
 *
 * (C) COPYRIGHT 2013 ARM Limited. All rights reserved.
 *
 * This program is free software and is provided to you under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation, and any use by you of this program is subject to the terms
 * of such GNU licence.
 *
 * A copy of the licence is included with the program, and can also be obtained
 * from Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 *
 */



/**
 * @file mali_kbase_pm_ca.c
 * Base kernel core availability APIs
 */

#include <kbase/src/common/mali_kbase.h>
#include <kbase/src/common/mali_kbase_pm.h>

extern const kbase_pm_ca_policy kbase_pm_ca_fixed_policy_ops;
#if MALI_CUSTOMER_RELEASE == 0
extern const kbase_pm_ca_policy kbase_pm_ca_random_policy_ops;
#endif

static const kbase_pm_ca_policy *const policy_list[] = {
	&kbase_pm_ca_fixed_policy_ops,
#if MALI_CUSTOMER_RELEASE == 0
	&kbase_pm_ca_random_policy_ops
#endif
};

/** The number of policies available in the system.
 *  This is derived from the number of functions listed in policy_get_functions.
 */
#define POLICY_COUNT (sizeof(policy_list)/sizeof(*policy_list))

mali_error kbase_pm_ca_init(kbase_device *kbdev)
{
	KBASE_DEBUG_ASSERT(kbdev != NULL);

	kbdev->pm.ca_current_policy = policy_list[0];

	kbdev->pm.ca_current_policy->init(kbdev);

	return MALI_ERROR_NONE;
}

void kbase_pm_ca_term(kbase_device *kbdev)
{
	kbdev->pm.ca_current_policy->term(kbdev);
}

int kbase_pm_ca_list_policies(const kbase_pm_ca_policy * const **list)
{
	if (!list)
		return POLICY_COUNT;

	*list = policy_list;

	return POLICY_COUNT;
}

KBASE_EXPORT_TEST_API(kbase_pm_ca_list_policies)

const kbase_pm_ca_policy *kbase_pm_ca_get_policy(kbase_device *kbdev)
{
	KBASE_DEBUG_ASSERT(kbdev != NULL);

	return kbdev->pm.ca_current_policy;
}

KBASE_EXPORT_TEST_API(kbase_pm_ca_get_policy)

void kbase_pm_ca_set_policy(kbase_device *kbdev, const kbase_pm_ca_policy *new_policy)
{
	const kbase_pm_ca_policy *old_policy;
	unsigned long flags;

	KBASE_DEBUG_ASSERT(kbdev != NULL);
	KBASE_DEBUG_ASSERT(new_policy != NULL);

	KBASE_TRACE_ADD(kbdev, PM_CA_SET_POLICY, NULL, NULL, 0u, new_policy->id);

	/* During a policy change we pretend the GPU is active */
	/* A suspend won't happen here, because we're in a syscall from a userspace thread */
	kbase_pm_context_active(kbdev);

	mutex_lock(&kbdev->pm.lock);

	/* Remove the policy to prevent IRQ handlers from working on it */
	spin_lock_irqsave(&kbdev->pm.power_change_lock, flags);
	old_policy = kbdev->pm.ca_current_policy;
	kbdev->pm.ca_current_policy = NULL;
	spin_unlock_irqrestore(&kbdev->pm.power_change_lock, flags);

	if (old_policy->term)
		old_policy->term(kbdev);

	if (new_policy->init)
		new_policy->init(kbdev);

	spin_lock_irqsave(&kbdev->pm.power_change_lock, flags);
	kbdev->pm.ca_current_policy = new_policy;

	/* If any core power state changes were previously attempted, but couldn't
	 * be made because the policy was changing (current_policy was NULL), then
	 * re-try them here. */
	kbase_pm_update_cores_state_nolock(kbdev);

	kbdev->pm.ca_current_policy->update_core_status(kbdev, kbdev->shader_ready_bitmap, kbdev->shader_transitioning_bitmap);

	spin_unlock_irqrestore(&kbdev->pm.power_change_lock, flags);

	mutex_unlock(&kbdev->pm.lock);

	/* Now the policy change is finished, we release our fake context active reference */
	kbase_pm_context_idle(kbdev);
}

KBASE_EXPORT_TEST_API(kbase_pm_ca_set_policy)

u64 kbase_pm_ca_get_core_mask(kbase_device *kbdev)
{
	lockdep_assert_held(&kbdev->pm.power_change_lock);

	/* All cores must be enabled when instrumentation is in use */
	if (kbdev->pm.instr_enabled == MALI_TRUE)
		return kbdev->shader_present_bitmap & kbdev->pm.debug_core_mask;

	if (kbdev->pm.ca_current_policy == NULL)
		return kbdev->shader_present_bitmap & kbdev->pm.debug_core_mask;

	return kbdev->pm.ca_current_policy->get_core_mask(kbdev) & kbdev->pm.debug_core_mask;
}

KBASE_EXPORT_TEST_API(kbase_pm_ca_get_core_mask)

void kbase_pm_ca_update_core_status(kbase_device *kbdev, u64 cores_ready, u64 cores_transitioning)
{
	lockdep_assert_held(&kbdev->pm.power_change_lock);

	if (kbdev->pm.ca_current_policy != NULL)
		kbdev->pm.ca_current_policy->update_core_status(kbdev, cores_ready, cores_transitioning);
}

void kbase_pm_ca_instr_enable(struct kbase_device *kbdev)
{
	unsigned long flags;

	spin_lock_irqsave(&kbdev->pm.power_change_lock, flags);
	kbdev->pm.instr_enabled = MALI_TRUE;

	kbase_pm_update_cores_state_nolock(kbdev);
	spin_unlock_irqrestore(&kbdev->pm.power_change_lock, flags);
}

void kbase_pm_ca_instr_disable(struct kbase_device *kbdev)
{
	unsigned long flags;

	spin_lock_irqsave(&kbdev->pm.power_change_lock, flags);
	kbdev->pm.instr_enabled = MALI_FALSE;

	kbase_pm_update_cores_state_nolock(kbdev);
	spin_unlock_irqrestore(&kbdev->pm.power_change_lock, flags);
}

