/*
 * Copyright 2024 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "shell/shell.h"

#include "apps/system_app_ids.h"
#include "kernel/pbl_malloc.h"
#include "process_management/app_install_manager.h"
#include "process_management/app_install_types.h"
#include "process_management/app_manager.h"
#include "services/common/compositor/compositor_transitions.h"

#define WATCHFACE_SHUTTER_COLOR GColorWhite
#define HEALTH_SHUTTER_COLOR PBL_IF_COLOR_ELSE(GColorBlack, GColorWhite)
#define ACTION_SHUTTER_COLOR PBL_IF_COLOR_ELSE(GColorLightGray, GColorWhite)

static const CompositorTransition *prv_get_watchface_compositor_animation(
    CompositorTransitionDirection direction) {
#if !PLATFORM_TINTIN
  return PBL_IF_RECT_ELSE(compositor_shutter_transition_get(direction, WATCHFACE_SHUTTER_COLOR),
                          compositor_port_hole_transition_app_get(direction));
#else
  return compositor_app_slide_transition_get(direction == CompositorTransitionDirectionRight);
#endif
}

#if !PLATFORM_TINTIN
static const CompositorTransition *prv_get_health_compositor_animation(
    CompositorTransitionDirection direction) {
  return PBL_IF_RECT_ELSE(compositor_shutter_transition_get(direction, HEALTH_SHUTTER_COLOR),
                          compositor_port_hole_transition_app_get(direction));
}
#endif

static const CompositorTransition *prv_get_action_compositor_animation(
    CompositorTransitionDirection direction) {
#if !PLATFORM_TINTIN
  return PBL_IF_RECT_ELSE(compositor_shutter_transition_get(direction, ACTION_SHUTTER_COLOR),
                          NULL);
#else
  return compositor_app_slide_transition_get(direction == CompositorTransitionDirectionRight);
#endif
}

const CompositorTransition *shell_get_watchface_compositor_animation(
    bool watchface_is_destination) {
  const CompositorTransitionDirection direction = watchface_is_destination ?
                CompositorTransitionDirectionLeft : CompositorTransitionDirectionRight;
  return prv_get_watchface_compositor_animation(direction);
}

static const CompositorTransition *prv_app_launcher_transition_animation(
    CompositorTransitionDirection direction) {
  const bool app_is_destination = (direction == CompositorTransitionDirectionRight);
#if !PLATFORM_TINTIN
  return PBL_IF_RECT_ELSE(compositor_launcher_app_transition_get(app_is_destination),
                          compositor_port_hole_transition_app_get(direction));
#else
  return compositor_app_slide_transition_get(app_is_destination);
#endif
}

const CompositorTransition *shell_get_close_compositor_animation(AppInstallId current_app_id,
                                                                 AppInstallId next_app_id) {
  const CompositorTransition *res = NULL;
  AppInstallEntry *app_entry = kernel_zalloc_check(sizeof(AppInstallEntry));

  if (app_install_get_entry_for_install_id(next_app_id, app_entry) &&
      app_install_entry_is_watchface(app_entry)) {
    if (current_app_id == APP_ID_LAUNCHER_MENU) {
      res = prv_get_watchface_compositor_animation(CompositorTransitionDirectionLeft);
      goto done;
#if !PLATFORM_TINTIN
    } else if (current_app_id == APP_ID_HEALTH_APP) {
      res = prv_get_health_compositor_animation(CompositorTransitionDirectionDown);
      goto done;
#endif
    } else {
      res = prv_get_action_compositor_animation(CompositorTransitionDirectionLeft);
      goto done;
    }
  }

  if (next_app_id == APP_ID_LAUNCHER_MENU) {
    res = prv_app_launcher_transition_animation(CompositorTransitionDirectionLeft);
    goto done;
  }

  // If we get here, we don't use a compositor animation for the transition

done:
  kernel_free(app_entry);
  return res;
}

const CompositorTransition *shell_get_open_compositor_animation(AppInstallId current_app_id,
                                                                AppInstallId next_app_id) {
  const CompositorTransition *res = NULL;
  AppInstallEntry *app_entry = kernel_zalloc_check(sizeof(AppInstallEntry));

  if (app_install_get_entry_for_install_id(current_app_id, app_entry)) {
    if (app_install_entry_is_watchface(app_entry)) {
      if (next_app_id == APP_ID_LAUNCHER_MENU) {
        res = prv_get_watchface_compositor_animation(CompositorTransitionDirectionRight);
        goto done;
#if !PLATFORM_TINTIN
      } else if (next_app_id == APP_ID_HEALTH_APP) {
        res = prv_get_health_compositor_animation(CompositorTransitionDirectionUp);
        goto done;
#endif
      }
#if !PLATFORM_TINTIN
    } else if ((current_app_id == APP_ID_HEALTH_APP) &&
               app_install_get_entry_for_install_id(next_app_id, app_entry) &&
               app_install_entry_is_watchface(app_entry)) {
      res = prv_get_health_compositor_animation(CompositorTransitionDirectionDown);
      goto done;
#endif
    }
  }

  if (current_app_id == APP_ID_LAUNCHER_MENU) {
    res = prv_app_launcher_transition_animation(CompositorTransitionDirectionRight);
    goto done;
  }

  // If we get here, we don't use a compositor animation for the transition

done:
  kernel_free(app_entry);
  return res;
}
