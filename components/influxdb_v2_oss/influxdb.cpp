#include "influxdb.h"

namespace esphome {
namespace influxdb {

#ifdef USE_BINARY_SENSOR
void BinarySensorField::publish(std::string &line) const {
  line += (this->sensor_->state ? 'T' : 'F');
}
#endif

#ifdef USE_SENSOR
void SensorField::publish(std::string &line) const {
  float state;

  if (this->raw_state_) {
    state = this->sensor_->get_raw_state();
  } else {
    state = this->sensor_->get_state();
  }

  switch (this->format_) {
  case SensorFieldFormat::Float:
    {
      char payload[esphome::VALUE_ACCURACY_MAX_LEN];
      size_t len = value_accuracy_to_buf(payload, state, static_cast<int8_t>(this->accuracy_decimals_));
      line += payload;
    }
    break;
  case SensorFieldFormat::Integer:
    line += str_sprintf("%ldi", std::lroundf(state));
    break;
  case SensorFieldFormat::UnsignedInteger:
    line += str_sprintf("%ldu", std::lroundf(std::abs(state)));
    break;
  }
}
#endif

#ifdef USE_TEXT_SENSOR
void TextSensorField::publish(std::string &line) const {
  line += '"';

  if (this->raw_state_) {
    line += this->sensor_->get_raw_state();
  } else {
    line += this->sensor_->get_state();
  }

  line += '"';
}
#endif

std::string Measurement::publish(const std::string &timestamp) const {
  std::string line{this->line_prefix_};
  char sensor_sep = ' ';

  for (const auto field : this->fields_) {
    if (!field->sensor_has_state()) {
      continue;
    }

    line += sensor_sep;

    if (!field->get_field_name().empty()) {
      line += field->get_field_name();
    } else {
      line += field->sensor_object_id();
    }

    line += '=';

    field->publish(line);

    sensor_sep = ',';
  }

  line += timestamp + '\n';

  return line;
}

void InfluxDB::setup() {
  http_request::Header header;

  header.name = "Content-Type";
  header.value = "text/plain; charset=utf-8";
  this->headers_.push_back(header);

  header.name = "Content-Encoding";
  header.value = "identity";
  this->headers_.push_back(header);

  header.name = "Accept";
  header.value = "application/json";
  this->headers_.push_back(header);

  if (!this->token_.empty()) {
    header.name = "Authorization";
    header.value = this->token_.c_str();
    this->headers_.push_back(header);
  }
}

#if ESPHOME_VERSION_CODE >= VERSION_CODE(2025, 7, 0)
void InfluxDB::loop() {
  this->disable_loop();
}
#endif

void InfluxDB::publish_action(const Measurement *measurement) {
  std::string timestamp;
  auto db = measurement->get_parent();

#ifdef USE_TIME
  if (db->clock_ != nullptr) {
    auto time = db->clock_->now();
    timestamp = str_sprintf(" %jd", (intmax_t) time.timestamp);
  }
#endif

  db->send_data(measurement->get_url(), measurement->publish(timestamp));
}

void InfluxDB::publish_batch_action(std::list<const Measurement *> measurements) {
  std::string timestamp;
  auto db = measurements.front()->get_parent();
  auto url = measurements.front()->get_url();
  std::string data;

#ifdef USE_TIME
  if (db->clock_ != nullptr) {
    auto time = db->clock_->now();
    timestamp = str_sprintf(" %jd", (intmax_t) time.timestamp);
  }
#endif

  for (auto measurement : measurements) {
    if (measurement->get_parent() != db) {
      ESP_LOGE(TAG, "Batch cannot include measurements for multiple databases.");
      continue;
    }

    if (measurement->get_url() != url) {
      ESP_LOGE(TAG, "Batch cannot include measurements for multiple buckets.");
      continue;
    }

    data += measurement->publish(timestamp);
  }

  db->send_data(url, std::move(data));
}

void InfluxDB::send_data(const std::string &url, std::string &&data) {
  uint8_t buf[1024];

  ESP_LOGD(TAG, "Publishing: %s", data.c_str());

  auto response = this->http_request_->post(url, data, this->headers_);

  if (response != nullptr && !this->http_request_->status_has_error()) {
    while (response->read(buf, sizeof(buf)) != 0) {}
  }

#ifdef USE_TIME
  if (this->backlog_max_depth_ != 0) {
    if (this->http_request_->status_has_error()) {
      if (this->backlog_.size() == this->backlog_max_depth_) {
	ESP_LOGW(TAG, "Backlog is full, dropping oldest entry.");
	this->backlog_.pop_front();
      }
      ESP_LOGD(TAG, "HTTP request failed, adding to backlog");
      this->backlog_.emplace_back(url, std::move(data));
      ESP_LOGD(TAG, "Backlog depth: %zd", this->backlog_.size());
    } else {
      if (!this->backlog_.empty()) {
	ESP_LOGD(TAG, "HTTP request succeeded, draining items from backlog");
	uint8_t item_count = 0;
	do {
	  const auto &m = this->backlog_.front();

	  auto response = this->http_request_->post(m.url, m.data, this->headers_);
	  if (response == nullptr || this->http_request_->status_has_error()) {
	    break;
	  }

	  while (response->read(buf, sizeof(buf)) != 0) {}
	  response->end();
	  this->backlog_.pop_front();
	  item_count++;

	} while (!this->backlog_.empty() && (item_count < this->backlog_drain_batch_));
	ESP_LOGD(TAG, "Drained %d items from backlog", item_count - 1);
      }
    }
  }
#endif

  if (response != nullptr) {
    response->end();
  }
}

}  // namespace influxdb
}  // namespace esphome
