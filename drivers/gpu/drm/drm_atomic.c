/*
 * Copyright (C) 2014 Red Hat
 * Copyright (C) 2014 Intel Corp.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 * Rob Clark <robdclark@gmail.com>
 * Daniel Vetter <daniel.vetter@ffwll.ch>
 */


#include <drm/drmP.h>
#include <drm/drm_atomic.h>
#include <drm/drm_mode.h>
#include <drm/drm_plane_helper.h>
#include <linux/sync_file.h>
#include <linux/cpu_input_boost.h>
#include <linux/devfreq_boost.h>

#include "drm_crtc_internal.h"

static void crtc_commit_free(struct kref *kref)
{
	struct drm_crtc_commit *commit =
		container_of(kref, struct drm_crtc_commit, ref);

	kfree(commit);
}

void drm_crtc_commit_put(struct drm_crtc_commit *commit)
{
	kref_put(&commit->ref, crtc_commit_free);
}
EXPORT_SYMBOL(drm_crtc_commit_put);

/**
 * drm_atomic_state_default_release -
 * release memory initialized by drm_atomic_state_init
 * @state: atomic state
 *
 * Free all the memory allocated by drm_atomic_state_init.
 * This is useful for drivers that subclass the atomic state.
 */
void drm_atomic_state_default_release(struct drm_atomic_state *state)
{
	kfree(state->connectors);
	kfree(state->crtcs);
	kfree(state->planes);
}
EXPORT_SYMBOL(drm_atomic_state_default_release);

/**
 * drm_atomic_state_init - init new atomic state
 * @dev: DRM device
 * @state: atomic state
 *
 * Default implementation for filling in a new atomic state.
 * This is useful for drivers that subclass the atomic state.
 */
int
drm_atomic_state_init(struct drm_device *dev, struct drm_atomic_state *state)
{
	/* TODO legacy paths should maybe do a better job about
	 * setting this appropriately?
	 */
	state->allow_modeset = true;

	state->crtcs = kcalloc(dev->mode_config.num_crtc,
			       sizeof(*state->crtcs), GFP_KERNEL);
	if (!state->crtcs)
		goto fail;
	state->planes = kcalloc(dev->mode_config.num_total_plane,
				sizeof(*state->planes), GFP_KERNEL);
	if (!state->planes)
		goto fail;

	state->dev = dev;

	DRM_DEBUG_ATOMIC("Allocated atomic state %p\n", state);

	return 0;
fail:
	drm_atomic_state_default_release(state);
	return -ENOMEM;
}
EXPORT_SYMBOL(drm_atomic_state_init);

/**
 * drm_atomic_state_alloc - allocate atomic state
 * @dev: DRM device
 *
 * This allocates an empty atomic state to track updates.
 */
struct drm_atomic_state *
drm_atomic_state_alloc(struct drm_device *dev)
{
	struct drm_mode_config *config = &dev->mode_config;
	struct drm_atomic_state *state;

	if (!config->funcs->atomic_state_alloc) {
		state = kzalloc(sizeof(*state), GFP_KERNEL);
		if (!state)
			return NULL;
		if (drm_atomic_state_init(dev, state) < 0) {
			kfree(state);
			return NULL;
		}
		return state;
	}

	return config->funcs->atomic_state_alloc(dev);
}
EXPORT_SYMBOL(drm_atomic_state_alloc);

/**
 * drm_atomic_state_default_clear - clear base atomic state
 * @state: atomic state
 *
 * Default implementation for clearing atomic state.
 * This is useful for drivers that subclass the atomic state.
 */
void drm_atomic_state_default_clear(struct drm_atomic_state *state)
{
	struct drm_device *dev = state->dev;
	struct drm_mode_config *config = &dev->mode_config;
	int i;

	DRM_DEBUG_ATOMIC("Clearing atomic state %p\n", state);

	for (i = 0; i < state->num_connector; i++) {
		struct drm_connector *connector = state->connectors[i].ptr;

		if (!connector)
			continue;

		connector->funcs->atomic_destroy_state(connector,
						       state->connectors[i].state);
		state->connectors[i].ptr = NULL;
		state->connectors[i].state = NULL;
		drm_connector_unreference(connector);
	}

	for (i = 0; i < config->num_crtc; i++) {
		struct drm_crtc *crtc = state->crtcs[i].ptr;

		if (!crtc)
			continue;

		crtc->funcs->atomic_destroy_state(crtc,
						  state->crtcs[i].state);

		if (state->crtcs[i].commit) {
			kfree(state->crtcs[i].commit->event);
			state->crtcs[i].commit->event = NULL;
			drm_crtc_commit_put(state->crtcs[i].commit);
		}

		state->crtcs[i].commit = NULL;
		state->crtcs[i].ptr = NULL;
		state->crtcs[i].state = NULL;
	}

	for (i = 0; i < config->num_total_plane; i++) {
		struct drm_plane *plane = state->planes[i].ptr;

		if (!plane)
			continue;

		plane->funcs->atomic_destroy_state(plane,
						   state->planes[i].state);
		state->planes[i].ptr = NULL;
		state->planes[i].state = NULL;
	}
}
EXPORT_SYMBOL(drm_atomic_state_default_clear);

/**
 * drm_atomic_state_clear - clear state object
 * @state: atomic state
 *
 * When the w/w mutex algorithm detects a deadlock we need to back off and drop
 * all locks. So someone else could sneak in and change the current modeset
 * configuration. Which means that all the state assembled in @state is no
 * longer an atomic update to the current state, but to some arbitrary earlier
 * state. Which could break assumptions the driver's ->atomic_check likely
 * relies on.
 *
 * Hence we must clear all cached state and completely start over, using this
 * function.
 */
void drm_atomic_state_clear(struct drm_atomic_state *state)
{
	struct drm_device *dev = state->dev;
	struct drm_mode_config *config = &dev->mode_config;

	if (config->funcs->atomic_state_clear)
		config->funcs->atomic_state_clear(state);
	else
		drm_atomic_state_default_clear(state);
}
EXPORT_SYMBOL(drm_atomic_state_clear);

/**
 * drm_atomic_state_free - free all memory for an atomic state
 * @state: atomic state to deallocate
 *
 * This frees all memory associated with an atomic state, including all the
 * per-object state for planes, crtcs and connectors.
 */
void drm_atomic_state_free(struct drm_atomic_state *state)
{
	struct drm_device *dev;
	struct drm_mode_config *config;

	if (!state)
		return;

	dev = state->dev;
	config = &dev->mode_config;

	drm_atomic_state_clear(state);

	DRM_DEBUG_ATOMIC("Freeing atomic state %p\n", state);

	if (config->funcs->atomic_state_free) {
		config->funcs->atomic_state_free(state);
	} else {
		drm_atomic_state_default_release(state);
		kfree(state);
	}
}
EXPORT_SYMBOL(drm_atomic_state_free);

/**
 * drm_atomic_get_crtc_state - get crtc state
 * @state: global atomic state object
 * @crtc: crtc to get state object for
 *
 * This function returns the crtc state for the given crtc, allocating it if
 * needed. It will also grab the relevant crtc lock to make sure that the state
 * is consistent.
 *
 * Returns:
 *
 * Either the allocated state or the error code encoded into the pointer. When
 * the error is EDEADLK then the w/w mutex code has detected a deadlock and the
 * entire atomic sequence must be restarted. All other errors are fatal.
 */
struct drm_crtc_state *
drm_atomic_get_crtc_state(struct drm_atomic_state *state,
			  struct drm_crtc *crtc)
{
	int ret, index = drm_crtc_index(crtc);
	struct drm_crtc_state *crtc_state;

	WARN_ON(!state->acquire_ctx);

	crtc_state = drm_atomic_get_existing_crtc_state(state, crtc);
	if (crtc_state)
		return crtc_state;

	ret = drm_modeset_lock(&crtc->mutex, state->acquire_ctx);
	if (ret)
		return ERR_PTR(ret);

	crtc_state = crtc->funcs->atomic_duplicate_state(crtc);
	if (!crtc_state)
		return ERR_PTR(-ENOMEM);

	state->crtcs[index].state = crtc_state;
	state->crtcs[index].ptr = crtc;
	crtc_state->state = state;

	DRM_DEBUG_ATOMIC("Added [CRTC:%d:%s] %p state to %p\n",
			 crtc->base.id, crtc->name, crtc_state, state);

	return crtc_state;
}
EXPORT_SYMBOL(drm_atomic_get_crtc_state);

static void set_out_fence_for_crtc(struct drm_atomic_state *state,
				   struct drm_crtc *crtc, s32 __user *fence_ptr)
{
	state->crtcs[drm_crtc_index(crtc)].out_fence_ptr = fence_ptr;
}

static s32 __user *get_out_fence_for_crtc(struct drm_atomic_state *state,
					  struct drm_crtc *crtc)
{
	s32 __user *fence_ptr;

	fence_ptr = state->crtcs[drm_crtc_index(crtc)].out_fence_ptr;
	state->crtcs[drm_crtc_index(crtc)].out_fence_ptr = NULL;

	return fence_ptr;
}

/**
 * drm_atomic_set_mode_for_crtc - set mode for CRTC
 * @state: the CRTC whose incoming state to update
 * @mode: kernel-internal mode to use for the CRTC, or NULL to disable
 *
 * Set a mode (originating from the kernel) on the desired CRTC state. Does
 * not change any other state properties, including enable, active, or
 * mode_changed.
 *
 * RETURNS:
 * Zero on success, error code on failure. Cannot return -EDEADLK.
 */
int drm_atomic_set_mode_for_crtc(struct drm_crtc_state *state,
				 struct drm_display_mode *mode)
{
	struct drm_mode_modeinfo umode;

	/* Early return for no change. */
	if (mode && memcmp(&state->mode, mode, sizeof(*mode)) == 0)
		return 0;

	drm_property_unreference_blob(state->mode_blob);
	state->mode_blob = NULL;

	if (mode) {
		drm_mode_convert_to_umode(&umode, mode);
		state->mode_blob =
			drm_property_create_blob(state->crtc->dev,
		                                 sizeof(umode),
		                                 &umode);
		if (IS_ERR(state->mode_blob))
			return PTR_ERR(state->mode_blob);

		drm_mode_copy(&state->mode, mode);
		state->enable = true;
		DRM_DEBUG_ATOMIC("Set [MODE:%s] for CRTC state %p\n",
				 mode->name, state);
	} else {
		memset(&state->mode, 0, sizeof(state->mode));
		state->enable = false;
		DRM_DEBUG_ATOMIC("Set [NOMODE] for CRTC state %p\n",
				 state);
	}

	return 0;
}
EXPORT_SYMBOL(drm_atomic_set_mode_for_crtc);

/**
 * drm_atomic_set_mode_prop_for_crtc - set mode for CRTC
 * @state: the CRTC whose incoming state to update
 * @blob: pointer to blob property to use for mode
 *
 * Set a mode (originating from a blob property) on the desired CRTC state.
 * This function will take a reference on the blob property for the CRTC state,
 * and release the reference held on the state's existing mode property, if any
 * was set.
 *
 * RETURNS:
 * Zero on success, error code on failure. Cannot return -EDEADLK.
 */
int drm_atomic_set_mode_prop_for_crtc(struct drm_crtc_state *state,
                                      struct drm_property_blob *blob)
{
	if (blob == state->mode_blob)
		return 0;

	drm_property_unreference_blob(state->mode_blob);
	state->mode_blob = NULL;

	memset(&state->mode, 0, sizeof(state->mode));

	if (blob) {
		if (blob->length != sizeof(struct drm_mode_modeinfo) ||
		    drm_mode_convert_umode(&state->mode,
		                           (const struct drm_mode_modeinfo *)
		                            blob->data))
			return -EINVAL;

		state->mode_blob = drm_property_reference_blob(blob);
		state->enable = true;
		DRM_DEBUG_ATOMIC("Set [MODE:%s] for CRTC state %p\n",
				 state->mode.name, state);
	} else {
		state->enable = false;
		DRM_DEBUG_ATOMIC("Set [NOMODE] for CRTC state %p\n",
				 state);
	}

	return 0;
}
EXPORT_SYMBOL(drm_atomic_set_mode_prop_for_crtc);

/**
 * drm_atomic_replace_property_blob - replace a blob property
 * @blob: a pointer to the member blob to be replaced
 * @new_blob: the new blob to replace with
 * @replaced: whether the blob has been replaced
 *
 * RETURNS:
 * Zero on success, error code on failure
 */
static void
drm_atomic_replace_property_blob(struct drm_property_blob **blob,
				 struct drm_property_blob *new_blob,
				 bool *replaced)
{
	struct drm_property_blob *old_blob = *blob;

	if (old_blob == new_blob)
		return;

	drm_property_unreference_blob(old_blob);
	if (new_blob)
		drm_property_reference_blob(new_blob);
	*blob = new_blob;
	*replaced = true;

	return;
}

static int
drm_atomic_replace_property_blob_from_id(struct drm_crtc *crtc,
					 struct drm_property_blob **blob,
					 uint64_t blob_id,
					 ssize_t expected_size,
					 bool *replaced)
{
	struct drm_property_blob *new_blob = NULL;

	if (blob_id != 0) {
		new_blob = drm_property_lookup_blob(crtc->dev, blob_id);
		if (new_blob == NULL)
			return -EINVAL;

		if (expected_size > 0 && expected_size != new_blob->length) {
			drm_property_unreference_blob(new_blob);
			return -EINVAL;
		}
	}

	drm_atomic_replace_property_blob(blob, new_blob, replaced);
	drm_property_unreference_blob(new_blob);

	return 0;
}

/**
 * drm_atomic_crtc_set_property - set property on CRTC
 * @crtc: the drm CRTC to set a property on
 * @state: the state object to update with the new property value
 * @property: the property to set
 * @val: the new property value
 *
 * Use this instead of calling crtc->atomic_set_property directly.
 * This function handles generic/core properties and calls out to
 * driver's ->atomic_set_property() for driver properties.  To ensure
 * consistent behavior you must call this function rather than the
 * driver hook directly.
 *
 * RETURNS:
 * Zero on success, error code on failure
 */
int drm_atomic_crtc_set_property(struct drm_crtc *crtc,
		struct drm_crtc_state *state, struct drm_property *property,
		uint64_t val)
{
	struct drm_device *dev = crtc->dev;
	struct drm_mode_config *config = &dev->mode_config;
	bool replaced = false;
	int ret;

	if (property == config->prop_active)
		state->active = val;
	else if (property == config->prop_mode_id) {
		struct drm_property_blob *mode =
			drm_property_lookup_blob(dev, val);
		ret = drm_atomic_set_mode_prop_for_crtc(state, mode);
		drm_property_unreference_blob(mode);
		return ret;
	} else if (property == config->degamma_lut_property) {
		ret = drm_atomic_replace_property_blob_from_id(crtc,
					&state->degamma_lut,
					val,
					-1,
					&replaced);
		state->color_mgmt_changed |= replaced;
		return ret;
	} else if (property == config->ctm_property) {
		ret = drm_atomic_replace_property_blob_from_id(crtc,
					&state->ctm,
					val,
					sizeof(struct drm_color_ctm),
					&replaced);
		state->color_mgmt_changed |= replaced;
		return ret;
	} else if (property == config->gamma_lut_property) {
		ret = drm_atomic_replace_property_blob_from_id(crtc,
					&state->gamma_lut,
					val,
					-1,
					&replaced);
		state->color_mgmt_changed |= replaced;
		return ret;
	} else if (property == config->prop_out_fence_ptr) {
		s32 __user *fence_ptr = u64_to_user_ptr(val);

		if (!fence_ptr)
			return 0;

		if (put_user(-1, fence_ptr))
			return -EFAULT;

		set_out_fence_for_crtc(state->state, crtc, fence_ptr);
	} else if (crtc->funcs->atomic_set_property)
		return crtc->funcs->atomic_set_property(crtc, state, property, val);
	else
		return -EINVAL;

	return 0;
}
EXPORT_SYMBOL(drm_atomic_crtc_set_property);

/**
 * drm_atomic_crtc_get_property - get property value from CRTC state
 * @crtc: the drm CRTC to set a property on
 * @state: the state object to get the property value from
 * @property: the property to set
 * @val: return location for the property value
 *
 * This function handles generic/core properties and calls out to
 * driver's ->atomic_get_property() for driver properties.  To ensure
 * consistent behavior you must call this function rather than the
 * driver hook directly.
 *
 * RETURNS:
 * Zero on success, error code on failure
 */
static int
drm_atomic_crtc_get_property(struct drm_crtc *crtc,
		const struct drm_crtc_state *state,
		struct drm_property *property, uint64_t *val)
{
	struct drm_device *dev = crtc->dev;
	struct drm_mode_config *config = &dev->mode_config;

	if (property == config->prop_active)
		*val = state->active;
	else if (property == config->prop_mode_id)
		*val = (state->mode_blob) ? state->mode_blob->base.id : 0;
	else if (property == config->degamma_lut_property)
		*val = (state->degamma_lut) ? state->degamma_lut->base.id : 0;
	else if (property == config->ctm_property)
		*val = (state->ctm) ? state->ctm->base.id : 0;
	else if (property == config->gamma_lut_property)
		*val = (state->gamma_lut) ? state->gamma_lut->base.id : 0;
	else if (property == config->prop_out_fence_ptr)
		*val = 0;
	else if (crtc->funcs->atomic_get_property)
		return crtc->funcs->atomic_get_property(crtc, state, property, val);
	else
		return -EINVAL;

	return 0;
}

/**
 * drm_atomic_crtc_check - check crtc state
 * @crtc: crtc to check
 * @state: crtc state to check
 *
 * Provides core sanity checks for crtc state.
 *
 * RETURNS:
 * Zero on success, error code on failure
 */
static int drm_atomic_crtc_check(struct drm_crtc *crtc,
		struct drm_crtc_state *state)
{
	/* NOTE: we explicitly don't enforce constraints such as primary
	 * layer covering entire screen, since that is something we want
	 * to allow (on hw that supports it).  For hw that does not, it
	 * should be checked in driver's crtc->atomic_check() vfunc.
	 *
	 * TODO: Add generic modeset state checks once we support those.
	 */

	if (state->active && !state->enable) {
		DRM_DEBUG_ATOMIC("[CRTC:%d:%s] active without enabled\n",
				 crtc->base.id, crtc->name);
		return -EINVAL;
	}

	/* The state->enable vs. state->mode_blob checks can be WARN_ON,
	 * as this is a kernel-internal detail that userspace should never
	 * be able to trigger. */
	if (drm_core_check_feature(crtc->dev, DRIVER_ATOMIC) &&
	    WARN_ON(state->enable && !state->mode_blob)) {
		DRM_DEBUG_ATOMIC("[CRTC:%d:%s] enabled without mode blob\n",
				 crtc->base.id, crtc->name);
		return -EINVAL;
	}

	if (drm_core_check_feature(crtc->dev, DRIVER_ATOMIC) &&
	    WARN_ON(!state->enable && state->mode_blob)) {
		DRM_DEBUG_ATOMIC("[CRTC:%d:%s] disabled with mode blob\n",
				 crtc->base.id, crtc->name);
		return -EINVAL;
	}

	/*
	 * Reject event generation for when a CRTC is off and stays off.
	 * It wouldn't be hard to implement this, but userspace has a track
	 * record of happily burning through 100% cpu (or worse, crash) when the
	 * display pipe is suspended. To avoid all that fun just reject updates
	 * that ask for events since likely that indicates a bug in the
	 * compositor's drawing loop. This is consistent with the vblank IOCTL
	 * and legacy page_flip IOCTL which also reject service on a disabled
	 * pipe.
	 */
	if (state->event && !state->active && !crtc->state->active) {
		DRM_DEBUG_ATOMIC("[CRTC:%d] requesting event but off\n",
				 crtc->base.id);
		return -EINVAL;
	}

	return 0;
}

/**
 * drm_atomic_get_plane_state - get plane state
 * @state: global atomic state object
 * @plane: plane to get state object for
 *
 * This function returns the plane state for the given plane, allocating it if
 * needed. It will also grab the relevant plane lock to make sure that the state
 * is consistent.
 *
 * Returns:
 *
 * Either the allocated state or the error code encoded into the pointer. When
 * the error is EDEADLK then the w/w mutex code has detected a deadlock and the
 * entire atomic sequence must be restarted. All other errors are fatal.
 */
struct drm_plane_state *
drm_atomic_get_plane_state(struct drm_atomic_state *state,
			  struct drm_plane *plane)
{
	int ret, index = drm_plane_index(plane);
	struct drm_plane_state *plane_state;

	WARN_ON(!state->acquire_ctx);

	plane_state = drm_atomic_get_existing_plane_state(state, plane);
	if (plane_state)
		return plane_state;

	ret = drm_modeset_lock(&plane->mutex, state->acquire_ctx);
	if (ret)
		return ERR_PTR(ret);

	plane_state = plane->funcs->atomic_duplicate_state(plane);
	if (!plane_state)
		return ERR_PTR(-ENOMEM);

	state->planes[index].state = plane_state;
	state->planes[index].ptr = plane;
	plane_state->state = state;

	DRM_DEBUG_ATOMIC("Added [PLANE:%d:%s] %p state to %p\n",
			 plane->base.id, plane->name, plane_state, state);

	if (plane_state->crtc) {
		struct drm_crtc_state *crtc_state;

		crtc_state = drm_atomic_get_crtc_state(state,
						       plane_state->crtc);
		if (IS_ERR(crtc_state))
			return ERR_CAST(crtc_state);
	}

	return plane_state;
}
EXPORT_SYMBOL(drm_atomic_get_plane_state);

/**
 * drm_atomic_plane_set_property - set property on plane
 * @plane: the drm plane to set a property on
 * @state: the state object to update with the new property value
 * @property: the property to set
 * @val: the new property value
 *
 * Use this instead of calling plane->atomic_set_property directly.
 * This function handles generic/core properties and calls out to
 * driver's ->atomic_set_property() for driver properties.  To ensure
 * consistent behavior you must call this function rather than the
 * driver hook directly.
 *
 * RETURNS:
 * Zero on success, error code on failure
 */
int drm_atomic_plane_set_property(struct drm_plane *plane,
		struct drm_plane_state *state, struct drm_property *property,
		uint64_t val)
{
	struct drm_device *dev = plane->dev;
	struct drm_mode_config *config = &dev->mode_config;

	if (property == config->prop_fb_id) {
		struct drm_framebuffer *fb = drm_framebuffer_lookup(dev, val);
		drm_atomic_set_fb_for_plane(state, fb);
		if (fb)
			drm_framebuffer_unreference(fb);
	} else if (property == config->prop_in_fence_fd) {
		if (state->fence)
			return -EINVAL;

		if (U642I64(val) == -1)
			return 0;

		state->fence = sync_file_get_fence(val);
		if (!state->fence)
			return -EINVAL;

	} else if (property == config->prop_crtc_id) {
		struct drm_crtc *crtc = drm_crtc_find(dev, val);
		return drm_atomic_set_crtc_for_plane(state, crtc);
	} else if (property == config->prop_crtc_x) {
		state->crtc_x = U642I64(val);
	} else if (property == config->prop_crtc_y) {
		state->crtc_y = U642I64(val);
	} else if (property == config->prop_crtc_w) {
		state->crtc_w = val;
	} else if (property == config->prop_crtc_h) {
		state->crtc_h = val;
	} else if (property == config->prop_src_x) {
		state->src_x = val;
	} else if (property == config->prop_src_y) {
		state->src_y = val;
	} else if (property == config->prop_src_w) {
		state->src_w = val;
	} else if (property == config->prop_src_h) {
		state->src_h = val;
	} else if (property == config->rotation_property) {
		state->rotation = val;
	} else if (property == plane->zpos_property) {
		state->zpos = val;
	} else if (plane->funcs->atomic_set_property) {
		return plane->funcs->atomic_set_property(plane, state,
				property, val);
	} else {
		return -EINVAL;
	}

	return 0;
}
EXPORT_SYMBOL(drm_atomic_plane_set_property);

/**
 * drm_atomic_plane_get_property - get property value from plane state
 * @plane: the drm plane to set a property on
 * @state: the state object to get the property value from
 * @property: the property to set
 * @val: return location for the property value
 *
 * This function handles generic/core properties and calls out to
 * driver's ->atomic_get_property() for driver properties.  To ensure
 * consistent behavior you must call this function rather than the
 * driver hook directly.
 *
 * RETURNS:
 * Zero on success, error code on failure
 */
static int
drm_atomic_plane_get_property(struct drm_plane *plane,
		const struct drm_plane_state *state,
		struct drm_property *property, uint64_t *val)
{
	struct drm_device *dev = plane->dev;
	struct drm_mode_config *config = &dev->mode_config;

	if (property == config->prop_fb_id) {
		*val = (state->fb) ? state->fb->base.id : 0;
	} else if (property == config->prop_in_fence_fd) {
		*val = -1;
	} else if (property == config->prop_crtc_id) {
		*val = (state->crtc) ? state->crtc->base.id : 0;
	} else if (property == config->prop_crtc_x) {
		*val = I642U64(state->crtc_x);
	} else if (property == config->prop_crtc_y) {
		*val = I642U64(state->crtc_y);
	} else if (property == config->prop_crtc_w) {
		*val = state->crtc_w;
	} else if (property == config->prop_crtc_h) {
		*val = state->crtc_h;
	} else if (property == config->prop_src_x) {
		*val = state->src_x;
	} else if (property == config->prop_src_y) {
		*val = state->src_y;
	} else if (property == config->prop_src_w) {
		*val = state->src_w;
	} else if (property == config->prop_src_h) {
		*val = state->src_h;
	} else if (property == config->rotation_property) {
		*val = state->rotation;
	} else if (property == plane->zpos_property) {
		*val = state->zpos;
	} else if (plane->funcs->atomic_get_property) {
		return plane->funcs->atomic_get_property(plane, state, property, val);
	} else {
		return -EINVAL;
	}

	return 0;
}

static bool
plane_switching_crtc(struct drm_atomic_state *state,
		     struct drm_plane *plane,
		     struct drm_plane_state *plane_state)
{
	if (!plane->state->crtc || !plane_state->crtc)
		return false;

	if (plane->state->crtc == plane_state->crtc)
		return false;

	/* This could be refined, but currently there's no helper or driver code
	 * to implement direct switching of active planes nor userspace to take
	 * advantage of more direct plane switching without the intermediate
	 * full OFF state.
	 */
	return true;
}

/**
 * drm_atomic_plane_check - check plane state
 * @plane: plane to check
 * @state: plane state to check
 *
 * Provides core sanity checks for plane state.
 *
 * RETURNS:
 * Zero on success, error code on failure
 */
static int drm_atomic_plane_check(struct drm_plane *plane,
		struct drm_plane_state *state)
{
	unsigned int fb_width, fb_height;
	int ret;

	/* either *both* CRTC and FB must be set, or neither */
	if (WARN_ON(state->crtc && !state->fb)) {
		DRM_DEBUG_ATOMIC("CRTC set but no FB\n");
		return -EINVAL;
	} else if (WARN_ON(state->fb && !state->crtc)) {
		DRM_DEBUG_ATOMIC("FB set but no CRTC\n");
		return -EINVAL;
	}

	/* if disabled, we don't care about the rest of the state: */
	if (!state->crtc)
		return 0;

	/* Check whether this plane is usable on this CRTC */
	if (!(plane->possible_crtcs & drm_crtc_mask(state->crtc))) {
		DRM_DEBUG_ATOMIC("Invalid crtc for plane\n");
		return -EINVAL;
	}

	/* Check whether this plane supports the fb pixel format. */
	ret = drm_plane_check_pixel_format(plane, state->fb->pixel_format);
	if (ret) {
		char *format_name = drm_get_format_name(state->fb->pixel_format);
		DRM_DEBUG_ATOMIC("Invalid pixel format %s\n", format_name);
		kfree(format_name);
		return ret;
	}

	/* Give drivers some help against integer overflows */
	if (state->crtc_w > INT_MAX ||
	    state->crtc_x > INT_MAX - (int32_t) state->crtc_w ||
	    state->crtc_h > INT_MAX ||
	    state->crtc_y > INT_MAX - (int32_t) state->crtc_h) {
		DRM_DEBUG_ATOMIC("Invalid CRTC coordinates %ux%u+%d+%d\n",
				 state->crtc_w, state->crtc_h,
				 state->crtc_x, state->crtc_y);
		return -ERANGE;
	}

	fb_width = state->fb->width << 16;
	fb_height = state->fb->height << 16;

	/* Make sure source coordinates are inside the fb. */
	if (state->src_w > fb_width ||
	    state->src_x > fb_width - state->src_w ||
	    state->src_h > fb_height ||
	    state->src_y > fb_height - state->src_h) {
		DRM_DEBUG_ATOMIC("Invalid source coordinates "
				 "%u.%06ux%u.%06u+%u.%06u+%u.%06u\n",
				 state->src_w >> 16, ((state->src_w & 0xffff) * 15625) >> 10,
				 state->src_h >> 16, ((state->src_h & 0xffff) * 15625) >> 10,
				 state->src_x >> 16, ((state->src_x & 0xffff) * 15625) >> 10,
				 state->src_y >> 16, ((state->src_y & 0xffff) * 15625) >> 10);
		return -ENOSPC;
	}

	if (plane_switching_crtc(state->state, plane, state)) {
		DRM_DEBUG_ATOMIC("[PLANE:%d:%s] switching CRTC directly\n",
				 plane->base.id, plane->name);
		return -EINVAL;
	}

	return 0;
}

/**
 * drm_atomic_get_connector_state - get connector state
 * @state: global atomic state object
 * @connector: connector to get state object for
 *
 * This function returns the connector state for the given connector,
 * allocating it if needed. It will also grab the relevant connector lock to
 * make sure that the state is consistent.
 *
 * Returns:
 *
 * Either the allocated state or the error code encoded into the pointer. When
 * the error is EDEADLK then the w/w mutex code has detected a deadlock and the
 * entire atomic sequence must be restarted. All other errors are fatal.
 */
struct drm_connector_state *
drm_atomic_get_connector_state(struct drm_atomic_state *state,
			  struct drm_connector *connector)
{
	int ret, index;
	struct drm_mode_config *config = &connector->dev->mode_config;
	struct drm_connector_state *connector_state;

	WARN_ON(!state->acquire_ctx);

	ret = drm_modeset_lock(&config->connection_mutex, state->acquire_ctx);
	if (ret)
		return ERR_PTR(ret);

	index = drm_connector_index(connector);

	if (index >= state->num_connector) {
		struct __drm_connnectors_state *c;
		int alloc = max(index + 1, config->num_connector);

		c = krealloc(state->connectors, alloc * sizeof(*state->connectors), GFP_KERNEL);
		if (!c)
			return ERR_PTR(-ENOMEM);

		state->connectors = c;
		memset(&state->connectors[state->num_connector], 0,
		       sizeof(*state->connectors) * (alloc - state->num_connector));

		state->num_connector = alloc;
	}

	if (state->connectors[index].state)
		return state->connectors[index].state;

	connector_state = connector->funcs->atomic_duplicate_state(connector);
	if (!connector_state)
		return ERR_PTR(-ENOMEM);

	drm_connector_reference(connector);
	state->connectors[index].state = connector_state;
	state->connectors[index].ptr = connector;
	connector_state->state = state;

	DRM_DEBUG_ATOMIC("Added [CONNECTOR:%d] %p state to %p\n",
			 connector->base.id, connector_state, state);

	if (connector_state->crtc) {
		struct drm_crtc_state *crtc_state;

		crtc_state = drm_atomic_get_crtc_state(state,
						       connector_state->crtc);
		if (IS_ERR(crtc_state))
			return ERR_CAST(crtc_state);
	}

	return connector_state;
}
EXPORT_SYMBOL(drm_atomic_get_connector_state);

/**
 * drm_atomic_connector_set_property - set property on connector.
 * @connector: the drm connector to set a property on
 * @state: the state object to update with the new property value
 * @property: the property to set
 * @val: the new property value
 *
 * Use this instead of calling connector->atomic_set_property directly.
 * This function handles generic/core properties and calls out to
 * driver's ->atomic_set_property() for driver properties.  To ensure
 * consistent behavior you must call this function rather than the
 * driver hook directly.
 *
 * RETURNS:
 * Zero on success, error code on failure
 */
int drm_atomic_connector_set_property(struct drm_connector *connector,
		struct drm_connector_state *state, struct drm_property *property,
		uint64_t val)
{
	struct drm_device *dev = connector->dev;
	struct drm_mode_config *config = &dev->mode_config;

	if (property == config->prop_crtc_id) {
		struct drm_crtc *crtc = drm_crtc_find(dev, val);
		return drm_atomic_set_crtc_for_connector(state, crtc);
	} else if (property == config->dpms_property) {
		/* setting DPMS property requires special handling, which
		 * is done in legacy setprop path for us.  Disallow (for
		 * now?) atomic writes to DPMS property:
		 */
		return -EINVAL;
	} else if (connector->funcs->atomic_set_property) {
		return connector->funcs->atomic_set_property(connector,
				state, property, val);
	} else {
		return -EINVAL;
	}
}
EXPORT_SYMBOL(drm_atomic_connector_set_property);

/**
 * drm_atomic_connector_get_property - get property value from connector state
 * @connector: the drm connector to set a property on
 * @state: the state object to get the property value from
 * @property: the property to set
 * @val: return location for the property value
 *
 * This function handles generic/core properties and calls out to
 * driver's ->atomic_get_property() for driver properties.  To ensure
 * consistent behavior you must call this function rather than the
 * driver hook directly.
 *
 * RETURNS:
 * Zero on success, error code on failure
 */
static int
drm_atomic_connector_get_property(struct drm_connector *connector,
		const struct drm_connector_state *state,
		struct drm_property *property, uint64_t *val)
{
	struct drm_device *dev = connector->dev;
	struct drm_mode_config *config = &dev->mode_config;

	if (property == config->prop_crtc_id) {
		*val = (state->crtc) ? state->crtc->base.id : 0;
	} else if (property == config->dpms_property) {
		*val = connector->dpms;
	} else if (connector->funcs->atomic_get_property) {
		return connector->funcs->atomic_get_property(connector,
				state, property, val);
	} else {
		return -EINVAL;
	}

	return 0;
}

int drm_atomic_get_property(struct drm_mode_object *obj,
		struct drm_property *property, uint64_t *val)
{
	struct drm_device *dev = property->dev;
	int ret;

	switch (obj->type) {
	case DRM_MODE_OBJECT_CONNECTOR: {
		struct drm_connector *connector = obj_to_connector(obj);
		WARN_ON(!drm_modeset_is_locked(&dev->mode_config.connection_mutex));
		ret = drm_atomic_connector_get_property(connector,
				connector->state, property, val);
		break;
	}
	case DRM_MODE_OBJECT_CRTC: {
		struct drm_crtc *crtc = obj_to_crtc(obj);
		WARN_ON(!drm_modeset_is_locked(&crtc->mutex));
		ret = drm_atomic_crtc_get_property(crtc,
				crtc->state, property, val);
		break;
	}
	case DRM_MODE_OBJECT_PLANE: {
		struct drm_plane *plane = obj_to_plane(obj);
		WARN_ON(!drm_modeset_is_locked(&plane->mutex));
		ret = drm_atomic_plane_get_property(plane,
				plane->state, property, val);
		break;
	}
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

/**
 * drm_atomic_set_crtc_for_plane - set crtc for plane
 * @plane_state: the plane whose incoming state to update
 * @crtc: crtc to use for the plane
 *
 * Changing the assigned crtc for a plane requires us to grab the lock and state
 * for the new crtc, as needed. This function takes care of all these details
 * besides updating the pointer in the state object itself.
 *
 * Returns:
 * 0 on success or can fail with -EDEADLK or -ENOMEM. When the error is EDEADLK
 * then the w/w mutex code has detected a deadlock and the entire atomic
 * sequence must be restarted. All other errors are fatal.
 */
int
drm_atomic_set_crtc_for_plane(struct drm_plane_state *plane_state,
			      struct drm_crtc *crtc)
{
	struct drm_plane *plane = plane_state->plane;
	struct drm_crtc_state *crtc_state;
	/* Nothing to do for same crtc*/
	if (plane_state->crtc == crtc)
		return 0;
	if (plane_state->crtc) {
		crtc_state = drm_atomic_get_crtc_state(plane_state->state,
						       plane_state->crtc);
		if (WARN_ON(IS_ERR(crtc_state)))
			return PTR_ERR(crtc_state);

		crtc_state->plane_mask &= ~(1 << drm_plane_index(plane));
	}

	plane_state->crtc = crtc;

	if (crtc) {
		crtc_state = drm_atomic_get_crtc_state(plane_state->state,
						       crtc);
		if (IS_ERR(crtc_state))
			return PTR_ERR(crtc_state);
		crtc_state->plane_mask |= (1 << drm_plane_index(plane));
	}

	if (crtc)
		DRM_DEBUG_ATOMIC("Link plane state %p to [CRTC:%d:%s]\n",
				 plane_state, crtc->base.id, crtc->name);
	else
		DRM_DEBUG_ATOMIC("Link plane state %p to [NOCRTC]\n",
				 plane_state);

	return 0;
}
EXPORT_SYMBOL(drm_atomic_set_crtc_for_plane);

/**
 * drm_atomic_set_fb_for_plane - set framebuffer for plane
 * @plane_state: atomic state object for the plane
 * @fb: fb to use for the plane
 *
 * Changing the assigned framebuffer for a plane requires us to grab a reference
 * to the new fb and drop the reference to the old fb, if there is one. This
 * function takes care of all these details besides updating the pointer in the
 * state object itself.
 */
void
drm_atomic_set_fb_for_plane(struct drm_plane_state *plane_state,
			    struct drm_framebuffer *fb)
{
	if (plane_state->fb)
		drm_framebuffer_unreference(plane_state->fb);
	if (fb)
		drm_framebuffer_reference(fb);
	plane_state->fb = fb;

	if (fb)
		DRM_DEBUG_ATOMIC("Set [FB:%d] for plane state %p\n",
				 fb->base.id, plane_state);
	else
		DRM_DEBUG_ATOMIC("Set [NOFB] for plane state %p\n",
				 plane_state);
}
EXPORT_SYMBOL(drm_atomic_set_fb_for_plane);

/**
 * drm_atomic_set_fence_for_plane - set fence for plane
 * @plane_state: atomic state object for the plane
 * @fence: fence to use for the plane
 *
 * Helper to setup the plane_state fence in case it is not set yet.
 * By using this drivers doesn't need to worry if the user choose
 * implicit or explicit fencing.
 *
 * This function will not set the fence to the state if it was set
 * via explicit fencing interfaces on the atomic ioctl. It will
 * all drope the reference to the fence as we not storing it
 * anywhere.
 *
 * Otherwise, if plane_state->fence is not set this function we
 * just set it with the received implict fence.
 */
void
drm_atomic_set_fence_for_plane(struct drm_plane_state *plane_state,
			       struct fence *fence)
{
	if (plane_state->fence) {
		fence_put(fence);
		return;
	}

	plane_state->fence = fence;
}
EXPORT_SYMBOL(drm_atomic_set_fence_for_plane);

/**
 * drm_atomic_set_crtc_for_connector - set crtc for connector
 * @conn_state: atomic state object for the connector
 * @crtc: crtc to use for the connector
 *
 * Changing the assigned crtc for a connector requires us to grab the lock and
 * state for the new crtc, as needed. This function takes care of all these
 * details besides updating the pointer in the state object itself.
 *
 * Returns:
 * 0 on success or can fail with -EDEADLK or -ENOMEM. When the error is EDEADLK
 * then the w/w mutex code has detected a deadlock and the entire atomic
 * sequence must be restarted. All other errors are fatal.
 */
int
drm_atomic_set_crtc_for_connector(struct drm_connector_state *conn_state,
				  struct drm_crtc *crtc)
{
	struct drm_crtc_state *crtc_state;

	if (conn_state->crtc == crtc)
		return 0;

	if (conn_state->crtc) {
		crtc_state = drm_atomic_get_existing_crtc_state(conn_state->state,
								conn_state->crtc);

		crtc_state->connector_mask &=
			~(1 << drm_connector_index(conn_state->connector));

		drm_connector_unreference(conn_state->connector);
		conn_state->crtc = NULL;
	}

	if (crtc) {
		crtc_state = drm_atomic_get_crtc_state(conn_state->state, crtc);
		if (IS_ERR(crtc_state))
			return PTR_ERR(crtc_state);

		crtc_state->connector_mask |=
			1 << drm_connector_index(conn_state->connector);

		drm_connector_reference(conn_state->connector);
		conn_state->crtc = crtc;

		DRM_DEBUG_ATOMIC("Link connector state %p to [CRTC:%d:%s]\n",
				 conn_state, crtc->base.id, crtc->name);
	} else {
		DRM_DEBUG_ATOMIC("Link connector state %p to [NOCRTC]\n",
				 conn_state);
	}

	return 0;
}
EXPORT_SYMBOL(drm_atomic_set_crtc_for_connector);

/**
 * drm_atomic_add_affected_connectors - add connectors for crtc
 * @state: atomic state
 * @crtc: DRM crtc
 *
 * This function walks the current configuration and adds all connectors
 * currently using @crtc to the atomic configuration @state. Note that this
 * function must acquire the connection mutex. This can potentially cause
 * unneeded seralization if the update is just for the planes on one crtc. Hence
 * drivers and helpers should only call this when really needed (e.g. when a
 * full modeset needs to happen due to some change).
 *
 * Returns:
 * 0 on success or can fail with -EDEADLK or -ENOMEM. When the error is EDEADLK
 * then the w/w mutex code has detected a deadlock and the entire atomic
 * sequence must be restarted. All other errors are fatal.
 */
int
drm_atomic_add_affected_connectors(struct drm_atomic_state *state,
				   struct drm_crtc *crtc)
{
	struct drm_mode_config *config = &state->dev->mode_config;
	struct drm_connector *connector;
	struct drm_connector_state *conn_state;
	int ret;

	ret = drm_modeset_lock(&config->connection_mutex, state->acquire_ctx);
	if (ret)
		return ret;

	DRM_DEBUG_ATOMIC("Adding all current connectors for [CRTC:%d:%s] to %p\n",
			 crtc->base.id, crtc->name, state);

	/*
	 * Changed connectors are already in @state, so only need to look at the
	 * current configuration.
	 */
	drm_for_each_connector(connector, state->dev) {
		if (connector->state->crtc != crtc)
			continue;

		conn_state = drm_atomic_get_connector_state(state, connector);
		if (IS_ERR(conn_state))
			return PTR_ERR(conn_state);
	}

	return 0;
}
EXPORT_SYMBOL(drm_atomic_add_affected_connectors);

/**
 * drm_atomic_add_affected_planes - add planes for crtc
 * @state: atomic state
 * @crtc: DRM crtc
 *
 * This function walks the current configuration and adds all planes
 * currently used by @crtc to the atomic configuration @state. This is useful
 * when an atomic commit also needs to check all currently enabled plane on
 * @crtc, e.g. when changing the mode. It's also useful when re-enabling a CRTC
 * to avoid special code to force-enable all planes.
 *
 * Since acquiring a plane state will always also acquire the w/w mutex of the
 * current CRTC for that plane (if there is any) adding all the plane states for
 * a CRTC will not reduce parallism of atomic updates.
 *
 * Returns:
 * 0 on success or can fail with -EDEADLK or -ENOMEM. When the error is EDEADLK
 * then the w/w mutex code has detected a deadlock and the entire atomic
 * sequence must be restarted. All other errors are fatal.
 */
int
drm_atomic_add_affected_planes(struct drm_atomic_state *state,
			       struct drm_crtc *crtc)
{
	struct drm_plane *plane;

	WARN_ON(!drm_atomic_get_existing_crtc_state(state, crtc));

	drm_for_each_plane_mask(plane, state->dev, crtc->state->plane_mask) {
		struct drm_plane_state *plane_state =
			drm_atomic_get_plane_state(state, plane);

		if (IS_ERR(plane_state))
			return PTR_ERR(plane_state);
	}
	return 0;
}
EXPORT_SYMBOL(drm_atomic_add_affected_planes);

/**
 * drm_atomic_legacy_backoff - locking backoff for legacy ioctls
 * @state: atomic state
 *
 * This function should be used by legacy entry points which don't understand
 * -EDEADLK semantics. For simplicity this one will grab all modeset locks after
 * the slowpath completed.
 */
void drm_atomic_legacy_backoff(struct drm_atomic_state *state)
{
	struct drm_device *dev = state->dev;
	unsigned crtc_mask = 0;
	struct drm_crtc *crtc;
	int ret;
	bool global = false;

	drm_for_each_crtc(crtc, dev) {
		if (crtc->acquire_ctx != state->acquire_ctx)
			continue;

		crtc_mask |= drm_crtc_mask(crtc);
		crtc->acquire_ctx = NULL;
	}

	if (WARN_ON(dev->mode_config.acquire_ctx == state->acquire_ctx)) {
		global = true;

		dev->mode_config.acquire_ctx = NULL;
	}

retry:
	drm_modeset_backoff(state->acquire_ctx);

	ret = drm_modeset_lock_all_ctx(dev, state->acquire_ctx);
	if (ret)
		goto retry;

	drm_for_each_crtc(crtc, dev)
		if (drm_crtc_mask(crtc) & crtc_mask)
			crtc->acquire_ctx = state->acquire_ctx;

	if (global)
		dev->mode_config.acquire_ctx = state->acquire_ctx;
}
EXPORT_SYMBOL(drm_atomic_legacy_backoff);

/**
 * drm_atomic_check_only - check whether a given config would work
 * @state: atomic configuration to check
 *
 * Note that this function can return -EDEADLK if the driver needed to acquire
 * more locks but encountered a deadlock. The caller must then do the usual w/w
 * backoff dance and restart. All other errors are fatal.
 *
 * Returns:
 * 0 on success, negative error code on failure.
 */
int drm_atomic_check_only(struct drm_atomic_state *state)
{
	struct drm_device *dev = state->dev;
	struct drm_mode_config *config = &dev->mode_config;
	struct drm_plane *plane;
	struct drm_plane_state *plane_state;
	struct drm_crtc *crtc;
	struct drm_crtc_state *crtc_state;
	int i, ret = 0;

	DRM_DEBUG_ATOMIC("checking %p\n", state);

	for_each_plane_in_state(state, plane, plane_state, i) {
		ret = drm_atomic_plane_check(plane, plane_state);
		if (ret) {
			DRM_DEBUG_ATOMIC("[PLANE:%d:%s] atomic core check failed\n",
					 plane->base.id, plane->name);
			return ret;
		}
	}

	for_each_crtc_in_state(state, crtc, crtc_state, i) {
		ret = drm_atomic_crtc_check(crtc, crtc_state);
		if (ret) {
			DRM_DEBUG_ATOMIC("[CRTC:%d:%s] atomic core check failed\n",
					 crtc->base.id, crtc->name);
			return ret;
		}
	}

	if (config->funcs->atomic_check)
		ret = config->funcs->atomic_check(state->dev, state);

	if (ret)
		return ret;

	if (!state->allow_modeset) {
		for_each_crtc_in_state(state, crtc, crtc_state, i) {
			if (drm_atomic_crtc_needs_modeset(crtc_state)) {
				DRM_DEBUG_ATOMIC("[CRTC:%d:%s] requires full modeset\n",
						 crtc->base.id, crtc->name);
				return -EINVAL;
			}
		}
	}

	return 0;
}
EXPORT_SYMBOL(drm_atomic_check_only);

/**
 * drm_atomic_commit - commit configuration atomically
 * @state: atomic configuration to check
 *
 * Note that this function can return -EDEADLK if the driver needed to acquire
 * more locks but encountered a deadlock. The caller must then do the usual w/w
 * backoff dance and restart. All other errors are fatal.
 *
 * Also note that on successful execution ownership of @state is transferred
 * from the caller of this function to the function itself. The caller must not
 * free or in any other way access @state. If the function fails then the caller
 * must clean up @state itself.
 *
 * Returns:
 * 0 on success, negative error code on failure.
 */
int drm_atomic_commit(struct drm_atomic_state *state)
{
	struct drm_mode_config *config = &state->dev->mode_config;
	int ret;

	ret = drm_atomic_check_only(state);
	if (ret)
		return ret;

	DRM_DEBUG_ATOMIC("commiting %p\n", state);

	return config->funcs->atomic_commit(state->dev, state, false);
}
EXPORT_SYMBOL(drm_atomic_commit);

/**
 * drm_atomic_nonblocking_commit - atomic&nonblocking configuration commit
 * @state: atomic configuration to check
 *
 * Note that this function can return -EDEADLK if the driver needed to acquire
 * more locks but encountered a deadlock. The caller must then do the usual w/w
 * backoff dance and restart. All other errors are fatal.
 *
 * Also note that on successful execution ownership of @state is transferred
 * from the caller of this function to the function itself. The caller must not
 * free or in any other way access @state. If the function fails then the caller
 * must clean up @state itself.
 *
 * Returns:
 * 0 on success, negative error code on failure.
 */
int drm_atomic_nonblocking_commit(struct drm_atomic_state *state)
{
	struct drm_mode_config *config = &state->dev->mode_config;
	int ret;

	ret = drm_atomic_check_only(state);
	if (ret)
		return ret;

	DRM_DEBUG_ATOMIC("commiting %p nonblocking\n", state);

	return config->funcs->atomic_commit(state->dev, state, true);
}
EXPORT_SYMBOL(drm_atomic_nonblocking_commit);

/*
 * The big monstor ioctl
 */

static struct drm_pending_vblank_event *create_vblank_event(
		struct drm_device *dev, uint64_t user_data)
{
	struct drm_pending_vblank_event *e = NULL;

	e = kzalloc(sizeof *e, GFP_KERNEL);
	if (!e)
		return NULL;

	e->event.base.type = DRM_EVENT_FLIP_COMPLETE;
	e->event.base.length = sizeof(e->event);
	e->event.user_data = user_data;

	return e;
}

static int atomic_set_prop(struct drm_atomic_state *state,
		struct drm_mode_object *obj, struct drm_property *prop,
		uint64_t prop_value)
{
	struct drm_mode_object *ref;
	int ret;

	if (!drm_property_change_valid_get(prop, prop_value, &ref))
		return -EINVAL;

	switch (obj->type) {
	case DRM_MODE_OBJECT_CONNECTOR: {
		struct drm_connector *connector = obj_to_connector(obj);
		struct drm_connector_state *connector_state;

		connector_state = drm_atomic_get_connector_state(state, connector);
		if (IS_ERR(connector_state)) {
			ret = PTR_ERR(connector_state);
			break;
		}

		ret = drm_atomic_connector_set_property(connector,
				connector_state, prop, prop_value);
		break;
	}
	case DRM_MODE_OBJECT_CRTC: {
		struct drm_crtc *crtc = obj_to_crtc(obj);
		struct drm_crtc_state *crtc_state;

		crtc_state = drm_atomic_get_crtc_state(state, crtc);
		if (IS_ERR(crtc_state)) {
			ret = PTR_ERR(crtc_state);
			break;
		}

		ret = drm_atomic_crtc_set_property(crtc,
				crtc_state, prop, prop_value);
		break;
	}
	case DRM_MODE_OBJECT_PLANE: {
		struct drm_plane *plane = obj_to_plane(obj);
		struct drm_plane_state *plane_state;

		plane_state = drm_atomic_get_plane_state(state, plane);
		if (IS_ERR(plane_state)) {
			ret = PTR_ERR(plane_state);
			break;
		}

		ret = drm_atomic_plane_set_property(plane,
				plane_state, prop, prop_value);
		break;
	}
	default:
		ret = -EINVAL;
		break;
	}

	drm_property_change_valid_put(prop, ref);
	return ret;
}

/**
 * drm_atomic_clean_old_fb -- Unset old_fb pointers and set plane->fb pointers.
 *
 * @dev: drm device to check.
 * @plane_mask: plane mask for planes that were updated.
 * @ret: return value, can be -EDEADLK for a retry.
 *
 * Before doing an update plane->old_fb is set to plane->fb,
 * but before dropping the locks old_fb needs to be set to NULL
 * and plane->fb updated. This is a common operation for each
 * atomic update, so this call is split off as a helper.
 */
void drm_atomic_clean_old_fb(struct drm_device *dev,
			     unsigned plane_mask,
			     int ret)
{
	struct drm_plane *plane;

	/* if succeeded, fixup legacy plane crtc/fb ptrs before dropping
	 * locks (ie. while it is still safe to deref plane->state).  We
	 * need to do this here because the driver entry points cannot
	 * distinguish between legacy and atomic ioctls.
	 */
	drm_for_each_plane_mask(plane, dev, plane_mask) {
		if (ret == 0) {
			struct drm_framebuffer *new_fb = plane->state->fb;
			if (new_fb)
				drm_framebuffer_reference(new_fb);
			plane->fb = new_fb;
			plane->crtc = plane->state->crtc;

			if (plane->old_fb)
				drm_framebuffer_unreference(plane->old_fb);
		}
		plane->old_fb = NULL;
	}
}
EXPORT_SYMBOL(drm_atomic_clean_old_fb);

/**
 * DOC: explicit fencing properties
 *
 * Explicit fencing allows userspace to control the buffer synchronization
 * between devices. A Fence or a group of fences are transfered to/from
 * userspace using Sync File fds and there are two DRM properties for that.
 * IN_FENCE_FD on each DRM Plane to send fences to the kernel and
 * OUT_FENCE_PTR on each DRM CRTC to receive fences from the kernel.
 *
 * As a contrast, with implicit fencing the kernel keeps track of any
 * ongoing rendering, and automatically ensures that the atomic update waits
 * for any pending rendering to complete. For shared buffers represented with
 * a struct &dma_buf this is tracked in &reservation_object structures.
 * Implicit syncing is how Linux traditionally worked (e.g. DRI2/3 on X.org),
 * whereas explicit fencing is what Android wants.
 *
 * "IN_FENCE_FD”:
 *	Use this property to pass a fence that DRM should wait on before
 *	proceeding with the Atomic Commit request and show the framebuffer for
 *	the plane on the screen. The fence can be either a normal fence or a
 *	merged one, the sync_file framework will handle both cases and use a
 *	fence_array if a merged fence is received. Passing -1 here means no
 *	fences to wait on.
 *
 *	If the Atomic Commit request has the DRM_MODE_ATOMIC_TEST_ONLY flag
 *	it will only check if the Sync File is a valid one.
 *
 *	On the driver side the fence is stored on the @fence parameter of
 *	struct &drm_plane_state. Drivers which also support implicit fencing
 *	should set the implicit fence using drm_atomic_set_fence_for_plane(),
 *	to make sure there's consistent behaviour between drivers in precedence
 *	of implicit vs. explicit fencing.
 *
 * "OUT_FENCE_PTR”:
 *	Use this property to pass a file descriptor pointer to DRM. Once the
 *	Atomic Commit request call returns OUT_FENCE_PTR will be filled with
 *	the file descriptor number of a Sync File. This Sync File contains the
 *	CRTC fence that will be signaled when all framebuffers present on the
 *	Atomic Commit * request for that given CRTC are scanned out on the
 *	screen.
 *
 *	The Atomic Commit request fails if a invalid pointer is passed. If the
 *	Atomic Commit request fails for any other reason the out fence fd
 *	returned will be -1. On a Atomic Commit with the
 *	DRM_MODE_ATOMIC_TEST_ONLY flag the out fence will also be set to -1.
 *
 *	Note that out-fences don't have a special interface to drivers and are
 *	internally represented by a struct &drm_pending_vblank_event in struct
 *	&drm_crtc_state, which is also used by the nonblocking atomic commit
 *	helpers and for the DRM event handling for existing userspace.
 */

struct drm_out_fence_state {
	s32 __user *out_fence_ptr;
	struct sync_file *sync_file;
	int fd;
};

static int setup_out_fence(struct drm_out_fence_state *fence_state,
			   struct fence *fence)
{
	fence_state->fd = get_unused_fd_flags(O_CLOEXEC);
	if (fence_state->fd < 0)
		return fence_state->fd;

	if (put_user(fence_state->fd, fence_state->out_fence_ptr))
		return -EFAULT;

	fence_state->sync_file = sync_file_create(fence);
	if (!fence_state->sync_file)
		return -ENOMEM;

	return 0;
}

static int prepare_crtc_signaling(struct drm_device *dev,
				  struct drm_atomic_state *state,
				  struct drm_mode_atomic *arg,
				  struct drm_file *file_priv,
				  struct drm_out_fence_state **fence_state,
				  unsigned int *num_fences)
{
	struct drm_crtc *crtc;
	struct drm_crtc_state *crtc_state;
	int i, ret;

	if (arg->flags & DRM_MODE_ATOMIC_TEST_ONLY)
		return 0;

	for_each_crtc_in_state(state, crtc, crtc_state, i) {
		s32 __user *fence_ptr;

		fence_ptr = get_out_fence_for_crtc(crtc_state->state, crtc);

		if (arg->flags & DRM_MODE_PAGE_FLIP_EVENT || fence_ptr) {
			struct drm_pending_vblank_event *e;

			e = create_vblank_event(dev, arg->user_data);
			if (!e)
				return -ENOMEM;

			crtc_state->event = e;
		}

		if (arg->flags & DRM_MODE_PAGE_FLIP_EVENT) {
			struct drm_pending_vblank_event *e = crtc_state->event;

			if (!file_priv)
				continue;

			ret = drm_event_reserve_init(dev, file_priv, &e->base,
						     &e->event.base);
			if (ret) {
				kfree(e);
				crtc_state->event = NULL;
				return ret;
			}
		}

		if (fence_ptr) {
			struct fence *fence;
			struct drm_out_fence_state *f;

			f = krealloc(*fence_state, sizeof(**fence_state) *
				     (*num_fences + 1), GFP_KERNEL);
			if (!f)
				return -ENOMEM;

			memset(&f[*num_fences], 0, sizeof(*f));

			f[*num_fences].out_fence_ptr = fence_ptr;
			*fence_state = f;

			fence = drm_crtc_create_fence(crtc);
			if (!fence)
				return -ENOMEM;

			ret = setup_out_fence(&f[(*num_fences)++], fence);
			if (ret) {
				fence_put(fence);
				return ret;
			}

			crtc_state->event->base.fence = fence;
		}
	}

	return 0;
}

static void complete_crtc_signaling(struct drm_device *dev,
				    struct drm_atomic_state *state,
				    struct drm_out_fence_state *fence_state,
				    unsigned int num_fences,
				    bool install_fds)
{
	struct drm_crtc *crtc;
	struct drm_crtc_state *crtc_state;
	int i;

	if (install_fds) {
		for (i = 0; i < num_fences; i++)
			fd_install(fence_state[i].fd,
				   fence_state[i].sync_file->file);

		kfree(fence_state);
		return;
	}

	for_each_crtc_in_state(state, crtc, crtc_state, i) {
		struct drm_pending_vblank_event *event = crtc_state->event;
		/*
		 * Free the allocated event. drm_atomic_helper_setup_commit
		 * can allocate an event too, so only free it if it's ours
		 * to prevent a double free in drm_atomic_state_clear.
		 */
		if (event && (event->base.fence || event->base.file_priv)) {
			drm_event_cancel_free(dev, &event->base);
			crtc_state->event = NULL;
		}
	}

	if (!fence_state)
		return;

	for (i = 0; i < num_fences; i++) {
		if (fence_state[i].sync_file)
			fput(fence_state[i].sync_file->file);
		if (fence_state[i].fd >= 0)
			put_unused_fd(fence_state[i].fd);

		/* If this fails log error to the user */
		if (fence_state[i].out_fence_ptr &&
		    put_user(-1, fence_state[i].out_fence_ptr))
			DRM_DEBUG_ATOMIC("Couldn't clear out_fence_ptr\n");
	}

	kfree(fence_state);
}

int drm_mode_atomic_ioctl(struct drm_device *dev,
			  void *data, struct drm_file *file_priv)
{
	struct drm_mode_atomic *arg = data;
	uint32_t __user *objs_ptr = (uint32_t __user *)(unsigned long)(arg->objs_ptr);
	uint32_t __user *count_props_ptr = (uint32_t __user *)(unsigned long)(arg->count_props_ptr);
	uint32_t __user *props_ptr = (uint32_t __user *)(unsigned long)(arg->props_ptr);
	uint64_t __user *prop_values_ptr = (uint64_t __user *)(unsigned long)(arg->prop_values_ptr);
	unsigned int copied_objs, copied_props;
	struct drm_atomic_state *state;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_plane *plane;
	struct drm_out_fence_state *fence_state;
	unsigned plane_mask;
	int ret = 0;
	unsigned int i, j, num_fences;

	/* disallow for drivers not supporting atomic: */
	if (!drm_core_check_feature(dev, DRIVER_ATOMIC))
		return -EINVAL;

	/* disallow for userspace that has not enabled atomic cap (even
	 * though this may be a bit overkill, since legacy userspace
	 * wouldn't know how to call this ioctl)
	 */
	if (!file_priv->atomic)
		return -EINVAL;

	if (arg->flags & ~DRM_MODE_ATOMIC_FLAGS)
		return -EINVAL;

	if (arg->reserved)
		return -EINVAL;

	if ((arg->flags & DRM_MODE_PAGE_FLIP_ASYNC) &&
			!dev->mode_config.async_page_flip)
		return -EINVAL;

	/* can't test and expect an event at the same time. */
	if ((arg->flags & DRM_MODE_ATOMIC_TEST_ONLY) &&
			(arg->flags & DRM_MODE_PAGE_FLIP_EVENT))
		return -EINVAL;

	if(!boost_gpu)
		if (!(arg->flags & DRM_MODE_ATOMIC_TEST_ONLY)) {
			cpu_input_boost_kick();
			devfreq_boost_kick(DEVFREQ_MSM_CPUBW);
			devfreq_boost_kick(DEVFREQ_MSM_LLCCBW);
		}

	drm_modeset_acquire_init(&ctx, 0);

	state = drm_atomic_state_alloc(dev);
	if (!state)
		return -ENOMEM;

	state->acquire_ctx = &ctx;
	state->allow_modeset = !!(arg->flags & DRM_MODE_ATOMIC_ALLOW_MODESET);

retry:
	plane_mask = 0;
	copied_objs = 0;
	copied_props = 0;
	fence_state = NULL;
	num_fences = 0;

	for (i = 0; i < arg->count_objs; i++) {
		uint32_t obj_id, count_props;
		struct drm_mode_object *obj;

		if (get_user(obj_id, objs_ptr + copied_objs)) {
			ret = -EFAULT;
			goto out;
		}

		obj = drm_mode_object_find(dev, obj_id, DRM_MODE_OBJECT_ANY);
		if (!obj) {
			ret = -ENOENT;
			goto out;
		}

		if (!obj->properties) {
			drm_mode_object_unreference(obj);
			ret = -ENOENT;
			goto out;
		}

		if (get_user(count_props, count_props_ptr + copied_objs)) {
			drm_mode_object_unreference(obj);
			ret = -EFAULT;
			goto out;
		}

		copied_objs++;

		for (j = 0; j < count_props; j++) {
			uint32_t prop_id;
			uint64_t prop_value;
			struct drm_property *prop;

			if (get_user(prop_id, props_ptr + copied_props)) {
				drm_mode_object_unreference(obj);
				ret = -EFAULT;
				goto out;
			}

			prop = drm_mode_obj_find_prop_id(obj, prop_id);
			if (!prop) {
				drm_mode_object_unreference(obj);
				ret = -ENOENT;
				goto out;
			}

			if (copy_from_user(&prop_value,
					   prop_values_ptr + copied_props,
					   sizeof(prop_value))) {
				drm_mode_object_unreference(obj);
				ret = -EFAULT;
				goto out;
			}

			ret = atomic_set_prop(state, obj, prop, prop_value);
			if (ret) {
				drm_mode_object_unreference(obj);
				goto out;
			}

			copied_props++;
		}

		if (obj->type == DRM_MODE_OBJECT_PLANE && count_props &&
		    !(arg->flags & DRM_MODE_ATOMIC_TEST_ONLY)) {
			plane = obj_to_plane(obj);
			plane_mask |= (1 << drm_plane_index(plane));
			plane->old_fb = plane->fb;
		}
		drm_mode_object_unreference(obj);
	}

	ret = prepare_crtc_signaling(dev, state, arg, file_priv, &fence_state,
				     &num_fences);
	if (ret)
		goto out;

	if (arg->flags & DRM_MODE_ATOMIC_TEST_ONLY) {
		/*
		 * Unlike commit, check_only does not clean up state.
		 * Below we call drm_atomic_state_free for it.
		 */
		ret = drm_atomic_check_only(state);
	} else if (arg->flags & DRM_MODE_ATOMIC_NONBLOCK) {
		ret = drm_atomic_nonblocking_commit(state);
	} else {
		ret = drm_atomic_commit(state);
	}

out:
	drm_atomic_clean_old_fb(dev, plane_mask, ret);

	complete_crtc_signaling(dev, state, fence_state, num_fences, !ret);

	if (ret == -EDEADLK) {
		drm_atomic_state_clear(state);
		drm_modeset_backoff(&ctx);
		goto retry;
	}

	if (ret || arg->flags & DRM_MODE_ATOMIC_TEST_ONLY)
		drm_atomic_state_free(state);

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);

	return ret;
}
