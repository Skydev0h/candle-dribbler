/*
 * candle-dribbler - ESP32 Zigbee light controller
 * Copyright 2023  Simon Arlott
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "nutt/light.h"

#include <esp_err.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_zigbee_cluster.h>
#include <esp_zigbee_type.h>
#include <nvs.h>
#include <nvs_handle.hpp>
#include <driver/gpio.h>
#include <ha/esp_zigbee_ha_standard.h>

#include <memory>
#include <string>
#include <thread>

#include "nutt/debounce.h"
#include "nutt/device.h"
#include "nutt/ui.h"

#ifndef ESP_ZB_HA_ON_OFF_LIGHT_SWITCH_DEVICE_ID
# define ESP_ZB_HA_ON_OFF_LIGHT_SWITCH_DEVICE_ID 0x0103
#endif

namespace nutt {

std::unique_ptr<nvs::NVSHandle> Light::nvs_;

Light::Light(size_t index, gpio_num_t switch_pin, bool switch_active_low,
		gpio_num_t relay_pin, bool relay_active_low) : index_(index),
		switch_debounce_(switch_pin, switch_active_low, DEBOUNCE_US),
		relay_pin_(relay_pin), relay_active_low_(relay_active_low),
		switch_active_(switch_debounce_.value()),
		persistent_enable_(enable_nvs()),
		temporary_enable_(persistent_enable_),
		primary_ep_(*new light::PrimaryEndpoint{*this}),
		secondary_ep_(*new light::SecondaryEndpoint{*this}),
		tertiary_ep_(*new light::TertiaryEndpoint{*this}),
		switch_status_ep_(*new light::SwitchStatusEndpoint{*this}),
		temporary_enable_ep_(*new light::TemporaryEnableEndpoint{*this}) {
	gpio_config_t relay_config = {
		.pin_bit_mask = 1ULL << relay_pin_,
		.mode = GPIO_MODE_OUTPUT,
		.pull_up_en = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE,
	};

	ESP_ERROR_CHECK(gpio_set_level(relay_pin_, relay_inactive()));
	ESP_ERROR_CHECK(gpio_config(&relay_config));

	ESP_LOGD(TAG, "Light %u switch is %d", index_, switch_active_);
}

bool Light::open_nvs() {
	if (!nvs_) {
		nvs_ = nvs::open_nvs_handle("light", NVS_READWRITE, nullptr);
	}

	return !!nvs_;
}

bool Light::enable_nvs() {
	if (open_nvs()) {
		uint8_t value = 1;

		if (nvs_->get_item(("e" + std::to_string(index_)).c_str(), value) == ESP_OK) {
			return value != 0;
		}
	}

	return true;
}

void Light::enable_nvs(bool state) {
	if (open_nvs()) {
		uint8_t value = state ? 1 : 0;

		nvs_->set_item(("e" + std::to_string(index_)).c_str(), value);
		nvs_->commit();
	}
}

void Light::attach(Device &device) {
	device_ = &device;
	device.add(*this, {
		primary_ep_,
		secondary_ep_,
		tertiary_ep_,
		switch_status_ep_,
		temporary_enable_ep_,
		*new light::PersistentEnableEndpoint{*this},
	});

	switch_debounce_.start(device);
}

unsigned long Light::run() {
	DebounceResult debounce = switch_debounce_.run();

	if (debounce.changed) {
		if (switch_debounce_.first()) {
			std::lock_guard lock{mutex_};

			switch_active_ = switch_debounce_.value();
			switch_change_us_ = esp_timer_get_time();
			request_refresh();
		} else {
			primary_switch(switch_debounce_.value(), true);
		}
	}

	return debounce.wait_ms;
}

bool Light::primary_on() const {
	std::lock_guard lock{mutex_};
	return primary_on_;
}

bool Light::secondary_on() const {
	std::lock_guard lock{mutex_};
	return secondary_on_;
}

bool Light::tertiary_on() const {
	std::lock_guard lock{mutex_};
	return tertiary_on_;
}

bool Light::switch_on() const {
	std::lock_guard lock{mutex_};
	return switch_active_;
}

bool Light::temporary_enable() const {
	std::lock_guard lock{mutex_};
	return temporary_enable_;
}

bool Light::persistent_enable() const {
	std::lock_guard lock{mutex_};
	return persistent_enable_;
}

bool Light::on() const {
	return on_;
}

void Light::primary_switch(bool state, bool local) {
	std::lock_guard lock{mutex_};

	ESP_LOGD(TAG, "Light %u set primary switch %d -> %d (%s)",
		index_, primary_on_, state, local ? "local" : "remote");

	primary_on_ = state;

	if (local) {
		if (switch_active_ != state) {
			uint64_t now_us = esp_timer_get_time();
			uint64_t elapsed_ms = (now_us - switch_change_us_) / 1000U;
			unsigned long days = 0;
			unsigned int hours, minutes, seconds, milliseconds;

			days = elapsed_ms / (86400U * 1000U);
			elapsed_ms %= 86400U * 1000U;

			hours = elapsed_ms / (3600U * 1000U);
			elapsed_ms %= 3600U * 1000U;

			minutes = elapsed_ms / (60U * 1000U);
			elapsed_ms %= 60U * 1000U;

			seconds = elapsed_ms / 1000U;
			elapsed_ms %= 1000U;

			milliseconds = elapsed_ms;

			ESP_LOGD(TAG, "Light %u switch was %s for %03lu+%02u:%02u:%02u.%03ums",
				index_, switch_active_ ? "on" : "off", days, hours, minutes,
				seconds, milliseconds);

			switch_active_ = state;
			switch_change_us_ = now_us;
		}
	}

	if (!state) {
		secondary_switch_locked(state, local);
	}

	update_state();
	device_->ui().light_switched(local);
}

void Light::secondary_switch(bool state, bool local) {
	std::lock_guard lock{mutex_};

	secondary_switch_locked(state, local);
	update_state();
	device_->ui().light_switched(local);
}

void Light::secondary_switch_locked(bool state, bool local) {
	ESP_LOGD(TAG, "Light %u set secondary switch %d -> %d (%s)",
		index_, secondary_on_, state, local ? "local" : "remote");
	secondary_on_ = state;
}

void Light::tertiary_switch(bool state) {
	std::lock_guard lock{mutex_};

	ESP_LOGD(TAG, "Light %u set tertiary switch %d -> %d",
		index_, tertiary_on_, state);
	tertiary_on_ = state;
	update_state();
	device_->ui().light_switched(false);
}

void Light::temporary_enable(bool state) {
	std::lock_guard lock{mutex_};

	ESP_LOGD(TAG, "Light %u set temporary enable %d -> %d",
		index_, temporary_enable_, state);
	temporary_enable_ = state;
	update_state();
}

void Light::persistent_enable(bool state) {
	std::lock_guard lock{mutex_};

	ESP_LOGD(TAG, "Light %u set persistent enable %d -> %d",
		index_, persistent_enable_, state);
	enable_nvs(state);
	persistent_enable_ = state;
	ESP_LOGD(TAG, "Light %u set temporary enable %d -> %d (auto)",
		index_, temporary_enable_, state);
	temporary_enable_ = state;
	update_state();
}

void Light::update_state() {
	bool on = (temporary_enable_ && primary_on_) || secondary_on_ || tertiary_on_;
	ESP_LOGI(TAG, "Light %u update state %d -> %d", index_, on_, on);
	on_ = on;
	gpio_set_level(relay_pin_, on_ ? relay_active() : relay_inactive());
	request_refresh();
}

void Light::request_refresh() {
	device_->request_refresh();
}

void Light::refresh() {
	primary_ep_.refresh();
	secondary_ep_.refresh();
	tertiary_ep_.refresh();
	switch_status_ep_.refresh();
	temporary_enable_ep_.refresh();
}

namespace light {

OnOffEndpoint::OnOffEndpoint(Light &light, ep_id_t base_ep_id,
		esp_zb_ha_standard_devices_t type, bool state)
		: ZigbeeEndpoint(base_ep_id + light.index(),
			ESP_ZB_AF_HA_PROFILE_ID, type),
		light_(light), state_(state) {
}

void OnOffEndpoint::configure_light_cluster_list(esp_zb_cluster_list_t &cluster_list) {
	esp_zb_on_off_cluster_cfg_t light_cfg{};
	light_cfg.on_off = state_;

	ESP_ERROR_CHECK(esp_zb_cluster_list_add_on_off_cluster(&cluster_list,
		esp_zb_on_off_cluster_create(&light_cfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));

	configure_common_cluster_list(cluster_list);
}

void OnOffEndpoint::configure_switch_cluster_list(esp_zb_cluster_list_t &cluster_list) {
	esp_zb_on_off_cluster_cfg_t switch_cfg{};
	switch_cfg.on_off = state_;

	ESP_ERROR_CHECK(esp_zb_cluster_list_add_on_off_cluster(&cluster_list,
		esp_zb_on_off_cluster_create(&switch_cfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));

	configure_common_cluster_list(cluster_list);
}

void OnOffEndpoint::configure_common_cluster_list(esp_zb_cluster_list_t &cluster_list) {
	ESP_ERROR_CHECK(esp_zb_cluster_list_add_groups_cluster(&cluster_list,
		esp_zb_groups_cluster_create(nullptr), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));

	ESP_ERROR_CHECK(esp_zb_cluster_list_add_scenes_cluster(&cluster_list,
		esp_zb_scenes_cluster_create(nullptr), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
}

PrimaryEndpoint::PrimaryEndpoint(Light &light)
		: OnOffEndpoint(light, BASE_EP_ID, ESP_ZB_HA_ON_OFF_LIGHT_DEVICE_ID,
			light.primary_on()) {
}

void PrimaryEndpoint::configure_cluster_list(esp_zb_cluster_list_t &cluster_list) {
	configure_light_cluster_list(cluster_list);
}

esp_err_t PrimaryEndpoint::set_attr_value(uint16_t cluster_id,
		uint16_t attr_id, const esp_zb_zcl_attribute_data_t *data) {
	if (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF) {
		if (attr_id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) {
			if (data->type == ESP_ZB_ZCL_ATTR_TYPE_BOOL
					&& data->size == sizeof(uint8_t)) {
				state_ = *(uint8_t *)data->value != 0;
				light_.primary_switch(state_, false);
				return ESP_OK;
			}
		}
	}
	return ESP_ERR_INVALID_ARG;
}

void PrimaryEndpoint::refresh() {
	bool new_state = light_.primary_on();

	if (new_state != state_) {
		uint8_t value = new_state ? 1 : 0;

		ESP_LOGD(TAG, "Light %u report primary switch %u", light_.index(), value);

		update_attr_value(ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
			ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
			ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID,
			&value);
		state_ = new_state;
	}
}

SecondaryEndpoint::SecondaryEndpoint(Light &light)
		: OnOffEndpoint(light, BASE_EP_ID, ESP_ZB_HA_ON_OFF_LIGHT_DEVICE_ID,
			light.secondary_on()) {
}

void SecondaryEndpoint::configure_cluster_list(esp_zb_cluster_list_t &cluster_list) {
	configure_light_cluster_list(cluster_list);
}

void SecondaryEndpoint::refresh() {
	bool new_state = light_.secondary_on();

	if (new_state != state_) {
		uint8_t value = new_state ? 1 : 0;

		ESP_LOGD(TAG, "Light %u report secondary switch %u", light_.index(), value);

		update_attr_value(ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
			ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
			ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID,
			&value);
		state_ = new_state;
	}
}

esp_err_t SecondaryEndpoint::set_attr_value(uint16_t cluster_id,
		uint16_t attr_id, const esp_zb_zcl_attribute_data_t *data) {
	if (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF) {
		if (attr_id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) {
			if (data->type == ESP_ZB_ZCL_ATTR_TYPE_BOOL
					&& data->size == sizeof(uint8_t)) {
				state_ = *(uint8_t *)data->value != 0;
				light_.secondary_switch(state_, false);
				return ESP_OK;
			}
		}
	}
	return ESP_ERR_INVALID_ARG;
}

TertiaryEndpoint::TertiaryEndpoint(Light &light)
		: OnOffEndpoint(light, BASE_EP_ID, ESP_ZB_HA_ON_OFF_LIGHT_DEVICE_ID,
			light.tertiary_on()) {
}

void TertiaryEndpoint::configure_cluster_list(esp_zb_cluster_list_t &cluster_list) {
	configure_light_cluster_list(cluster_list);
}

void TertiaryEndpoint::refresh() {
	bool new_state = light_.tertiary_on();

	if (new_state != state_) {
		uint8_t value = new_state ? 1 : 0;

		ESP_LOGD(TAG, "Light %u report tertiary switch %u", light_.index(), value);

		update_attr_value(ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
			ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
			ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID,
			&value);
		state_ = new_state;
	}
}

esp_err_t TertiaryEndpoint::set_attr_value(uint16_t cluster_id,
		uint16_t attr_id, const esp_zb_zcl_attribute_data_t *data) {
	if (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF) {
		if (attr_id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) {
			if (data->type == ESP_ZB_ZCL_ATTR_TYPE_BOOL
					&& data->size == sizeof(uint8_t)) {
				state_ = *(uint8_t *)data->value != 0;
				light_.tertiary_switch(state_);
				return ESP_OK;
			}
		}
	}
	return ESP_ERR_INVALID_ARG;
}

uint32_t SwitchStatusEndpoint::type_{
	  (  0x03 << 24)  /* Group: Binary Input            */
	| (  0x00 << 16)  /* Type:  Application Domain HVAC */
	|  0x004B         /* Index: Lighting Status BI      */
};

SwitchStatusEndpoint::SwitchStatusEndpoint(Light &light)
		: ZigbeeEndpoint(BASE_EP_ID + light.index(),
			ESP_ZB_AF_HA_PROFILE_ID, ESP_ZB_HA_ON_OFF_LIGHT_SWITCH_DEVICE_ID),
		light_(light), state_(light_.switch_on() ? 1 : 0) {
}

void SwitchStatusEndpoint::configure_cluster_list(esp_zb_cluster_list_t &cluster_list) {
	esp_zb_binary_input_cluster_cfg_t input_cfg = {
		.out_of_service = 0,
		.status_flags = 0,
	};

	esp_zb_attribute_list_t *input_cluster = esp_zb_binary_input_cluster_create(&input_cfg);

	ESP_ERROR_CHECK(esp_zb_binary_input_cluster_add_attr(input_cluster,
			ESP_ZB_ZCL_ATTR_BINARY_INPUT_PRESENT_VALUE_ID, &state_));

	ESP_ERROR_CHECK(esp_zb_binary_input_cluster_add_attr(input_cluster,
			ESP_ZB_ZCL_ATTR_BINARY_INPUT_APPLICATION_TYPE_ID, &type_));

	ESP_ERROR_CHECK(esp_zb_binary_input_cluster_add_attr(input_cluster,
			ESP_ZB_ZCL_ATTR_BINARY_INPUT_DESCRIPTION_ID,
			ZigbeeString("Light " + std::to_string(light_.index())).data()));

	ESP_ERROR_CHECK(esp_zb_cluster_list_add_binary_input_cluster(&cluster_list,
		input_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
}

void SwitchStatusEndpoint::refresh() {
	uint8_t new_state = light_.switch_on() ? 1 : 0;

	if (new_state != state_) {
		ESP_LOGD(TAG, "Light %u report switch state %u", light_.index(), new_state);

		update_attr_value(ESP_ZB_ZCL_CLUSTER_ID_BINARY_INPUT,
			ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
			ESP_ZB_ZCL_ATTR_BINARY_INPUT_PRESENT_VALUE_ID,
			&new_state);
		state_ = new_state;
	}
}

esp_err_t SwitchStatusEndpoint::set_attr_value(uint16_t cluster_id,
		uint16_t attr_id, const esp_zb_zcl_attribute_data_t *data) {
	if (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF) {
		if (attr_id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) {
			if (data->type == ESP_ZB_ZCL_ATTR_TYPE_BOOL
					&& data->size == sizeof(uint8_t)) {
				state_ = *(uint8_t *)data->value != 0;
				light_.request_refresh();
				return ESP_OK;
			}
		}
	}
	return ESP_ERR_INVALID_ARG;
}

TemporaryEnableEndpoint::TemporaryEnableEndpoint(Light &light)
		: OnOffEndpoint(light, BASE_EP_ID, ESP_ZB_HA_CONFIGURATION_TOOL_DEVICE_ID,
			light.temporary_enable()) {
}

void TemporaryEnableEndpoint::configure_cluster_list(esp_zb_cluster_list_t &cluster_list) {
	configure_switch_cluster_list(cluster_list);
}

void TemporaryEnableEndpoint::refresh() {
	bool new_state = light_.temporary_enable();

	if (new_state != state_) {
		uint8_t value = new_state ? 1 : 0;

		ESP_LOGD(TAG, "Light %u report temporary enable %u", light_.index(), value);

		update_attr_value(ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
			ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
			ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID,
			&value);
		state_ = new_state;
	}
}

esp_err_t TemporaryEnableEndpoint::set_attr_value(uint16_t cluster_id,
		uint16_t attr_id, const esp_zb_zcl_attribute_data_t *data) {
	if (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF) {
		if (attr_id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) {
			if (data->type == ESP_ZB_ZCL_ATTR_TYPE_BOOL
					&& data->size == sizeof(uint8_t)) {
				state_ = *(uint8_t *)data->value != 0;
				light_.temporary_enable(state_);
				return ESP_OK;
			}
		}
	}
	return ESP_ERR_INVALID_ARG;
}

PersistentEnableEndpoint::PersistentEnableEndpoint(Light &light)
		: OnOffEndpoint(light, BASE_EP_ID, ESP_ZB_HA_CONFIGURATION_TOOL_DEVICE_ID,
			light.persistent_enable()) {
}

void PersistentEnableEndpoint::configure_cluster_list(esp_zb_cluster_list_t &cluster_list) {
	configure_switch_cluster_list(cluster_list);
}

esp_err_t PersistentEnableEndpoint::set_attr_value(uint16_t cluster_id,
		uint16_t attr_id, const esp_zb_zcl_attribute_data_t *data) {
	if (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF) {
		if (attr_id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) {
			if (data->type == ESP_ZB_ZCL_ATTR_TYPE_BOOL
					&& data->size == sizeof(uint8_t)) {
				state_ = *(uint8_t *)data->value != 0;
				light_.persistent_enable(state_);
				return ESP_OK;
			}
		}
	}
	return ESP_ERR_INVALID_ARG;
}

} // namespace light

} // namespace nutt
