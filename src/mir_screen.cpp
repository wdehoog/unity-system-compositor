/*
 * Copyright © 2014-2015 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "mir_screen.h"
#include "clock.h"

#include <mir/main_loop.h>
#include <mir/time/alarm_factory.h>
#include <mir/compositor/compositor.h>
#include <mir/graphics/display.h>
#include <mir/graphics/display_configuration.h>
#include <mir/input/touch_visualizer.h>

#include <cstdio>
#include "screen_hardware.h"
#include "power_state_change_reason.h"
#include "server.h"

namespace mi = mir::input;
namespace mg = mir::graphics;

usc::MirScreen::MirScreen(
    std::shared_ptr<usc::ScreenHardware> const& screen_hardware,
    std::shared_ptr<mir::compositor::Compositor> const& compositor,
    std::shared_ptr<mir::graphics::Display> const& display,
    std::shared_ptr<mir::input::TouchVisualizer> const& touch_visualizer,
    std::shared_ptr<mir::time::AlarmFactory> const& alarm_factory,
    std::shared_ptr<usc::Clock> const& clock,
    Timeouts inactivity_timeouts,
    Timeouts notification_timeouts)
    : screen_hardware{screen_hardware},
      compositor{compositor},
      display{display},
      touch_visualizer{touch_visualizer},
      alarm_factory{alarm_factory},
      clock{clock},
      power_off_alarm{alarm_factory->create_alarm(
              std::bind(&usc::MirScreen::power_off_alarm_notification, this))},
      dimmer_alarm{alarm_factory->create_alarm(
              std::bind(&usc::MirScreen::dimmer_alarm_notification, this))},
      inactivity_timeouts(inactivity_timeouts),
      notification_timeouts(notification_timeouts),
      current_power_mode{MirPowerMode::mir_power_mode_on},
      restart_timers{true},
      power_state_change_handler{[](MirPowerMode,PowerStateChangeReason){}},
      allow_proximity_to_turn_on_screen{false}
{
    /*
     * Make sure the compositor is running as certain conditions can
     * cause Mir to tear down the compositor threads before we get
     * to this point. See bug #1410381.
     */
    compositor->start();
    reset_timers_l(PowerStateChangeReason::inactivity);
}

usc::MirScreen::~MirScreen() = default;

void usc::MirScreen::keep_display_on_temporarily()
{
    std::lock_guard<std::mutex> lock{guard};
    reset_timers_l(PowerStateChangeReason::inactivity);
    if (current_power_mode == MirPowerMode::mir_power_mode_on)
    {
        screen_hardware->set_normal_backlight();
        screen_hardware->enable_proximity(false);
    }
}

void usc::MirScreen::enable_inactivity_timers(bool enable)
{
    std::lock_guard<std::mutex> lock{guard};
    enable_inactivity_timers_l(enable);
}

MirPowerMode usc::MirScreen::get_screen_power_mode()
{
    std::lock_guard<std::mutex> lock{guard};
    return current_power_mode;
}

void usc::MirScreen::set_screen_power_mode(MirPowerMode mode, PowerStateChangeReason reason)
{
    std::lock_guard<std::mutex> lock{guard};
    set_screen_power_mode_l(mode, reason);
}

void usc::MirScreen::keep_display_on(bool on)
{
    std::lock_guard<std::mutex> lock{guard};
    restart_timers = !on;
    enable_inactivity_timers_l(!on);

    if (on)
        set_screen_power_mode_l(MirPowerMode::mir_power_mode_on, PowerStateChangeReason::unknown);
}

void usc::MirScreen::set_brightness(int brightness)
{
    std::lock_guard<std::mutex> lock{guard};
    screen_hardware->set_brightness(brightness);
}

void usc::MirScreen::enable_auto_brightness(bool enable)
{
    std::lock_guard<std::mutex> lock{guard};
    screen_hardware->enable_auto_brightness(enable);
}

void usc::MirScreen::set_inactivity_timeouts(int raw_poweroff_timeout, int raw_dimmer_timeout)
{
    std::lock_guard<std::mutex> lock{guard};

    std::chrono::seconds the_power_off_timeout{raw_poweroff_timeout};
    std::chrono::seconds the_dimming_timeout{raw_dimmer_timeout};

    if (raw_poweroff_timeout >= 0)
        inactivity_timeouts.power_off_timeout = std::chrono::duration_cast<std::chrono::milliseconds>(the_power_off_timeout);

    if (raw_dimmer_timeout >= 0)
        inactivity_timeouts.dimming_timeout = std::chrono::duration_cast<std::chrono::milliseconds>(the_dimming_timeout);

    cancel_timers_l(PowerStateChangeReason::inactivity);
    reset_timers_l(PowerStateChangeReason::inactivity);
}

void usc::MirScreen::set_screen_power_mode_l(MirPowerMode mode, PowerStateChangeReason reason)
{
    if (!is_screen_change_allowed(mode, reason))
        return;

    // Notifications don't turn on the screen directly, they rely on proximity events
    if (mode == MirPowerMode::mir_power_mode_on &&
        reason == PowerStateChangeReason::notification)
    {
        if (current_power_mode != MirPowerMode::mir_power_mode_on)
        {
            allow_proximity_to_turn_on_screen = true;
            screen_hardware->enable_proximity(true);
        }
        reset_timers_ignoring_power_mode_l(reason);
        return;
    }

    if (mode == MirPowerMode::mir_power_mode_on)
    {
        /* The screen may be dim, but on - make sure to reset backlight */
        if (current_power_mode == MirPowerMode::mir_power_mode_on)
            screen_hardware->set_normal_backlight();
        configure_display_l(mode, reason);
        reset_timers_l(reason);
    }
    else
    {
        cancel_timers_l(reason);
        configure_display_l(mode, reason);
    }
}

void usc::MirScreen::configure_display_l(MirPowerMode mode, PowerStateChangeReason reason)
{
    if (reason != PowerStateChangeReason::proximity)
    {
        screen_hardware->enable_proximity(false);
        allow_proximity_to_turn_on_screen = false;
    }

    if (current_power_mode == mode)
        return;

    allow_proximity_to_turn_on_screen =
        mode == mir_power_mode_off &&
        reason == PowerStateChangeReason::proximity;

    std::shared_ptr<mg::DisplayConfiguration> displayConfig = display->configuration();

    displayConfig->for_each_output(
        [&](const mg::UserDisplayConfigurationOutput displayConfigOutput) {
            displayConfigOutput.power_mode = mode;
        }
    );

    compositor->stop();

    bool const power_on = mode == MirPowerMode::mir_power_mode_on;
    if (power_on)
    {
        //Some devices do not turn screen on properly from suspend mode
        screen_hardware->disable_suspend();
    }
    else
    {
        screen_hardware->turn_off_backlight();
    }

    display->configure(*displayConfig.get());

    if (power_on)
    {
        compositor->start();
        screen_hardware->set_normal_backlight();
    }

    current_power_mode = mode;

    // TODO: Don't call this under lock
    power_state_change_handler(mode, reason);

    if (!power_on)
        screen_hardware->allow_suspend();
}

void usc::MirScreen::cancel_timers_l(PowerStateChangeReason reason)
{
    if (reason == PowerStateChangeReason::proximity)
        return;

    power_off_alarm->cancel();
    dimmer_alarm->cancel();
    next_power_off = {};
    next_dimming = {};
}

void usc::MirScreen::reset_timers_l(PowerStateChangeReason reason)
{
    if (current_power_mode != MirPowerMode::mir_power_mode_off)
        reset_timers_ignoring_power_mode_l(reason);
}

void usc::MirScreen::reset_timers_ignoring_power_mode_l(PowerStateChangeReason reason)
{
    if (!restart_timers || reason == PowerStateChangeReason::proximity)
        return;

    auto const timeouts = timeouts_for(reason);
    auto const now = clock->now();

    if (timeouts.power_off_timeout.count() > 0)
    {
        auto const new_next_power_off = now + timeouts.power_off_timeout;
        if (new_next_power_off > next_power_off)
        {
            power_off_alarm->reschedule_in(timeouts.power_off_timeout);
            next_power_off = new_next_power_off;
        }
    }

    if (timeouts.dimming_timeout.count() > 0)
    {
        auto const new_next_dimming = now + timeouts.dimming_timeout;
        if (new_next_dimming > next_dimming)
        {
            dimmer_alarm->reschedule_in(timeouts.dimming_timeout);
            next_dimming = new_next_dimming;
        }
    }
}

void usc::MirScreen::enable_inactivity_timers_l(bool enable)
{
    if (enable)
        reset_timers_l(PowerStateChangeReason::inactivity);
    else
        cancel_timers_l(PowerStateChangeReason::inactivity);
}

usc::MirScreen::Timeouts usc::MirScreen::timeouts_for(PowerStateChangeReason reason)
{
    if (reason == PowerStateChangeReason::notification)
        return notification_timeouts;
    else
        return inactivity_timeouts;
}

bool usc::MirScreen::is_screen_change_allowed(MirPowerMode mode, PowerStateChangeReason reason)
{
    if (mode == MirPowerMode::mir_power_mode_on &&
        reason == PowerStateChangeReason::proximity &&
        !allow_proximity_to_turn_on_screen)
    {
        return false;
    }

    return true;
}

void usc::MirScreen::power_off_alarm_notification()
{
    std::lock_guard<std::mutex> lock{guard};
    configure_display_l(MirPowerMode::mir_power_mode_off, PowerStateChangeReason::inactivity);
    next_power_off = {};
}

void usc::MirScreen::dimmer_alarm_notification()
{
    std::lock_guard<std::mutex> lock{guard};
    screen_hardware->set_dim_backlight();
    next_dimming = {};
}

void usc::MirScreen::set_touch_visualization_enabled(bool enabled)
{
    std::lock_guard<std::mutex> lock{guard};

    if (enabled)
        touch_visualizer->enable();
    else
        touch_visualizer->disable();
}

void usc::MirScreen::register_power_state_change_handler(
    PowerStateChangeHandler const& handler)
{
    std::lock_guard<std::mutex> lock{guard};

    power_state_change_handler = handler;
}