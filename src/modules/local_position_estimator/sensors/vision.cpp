#include "../BlockLocalPositionEstimator.hpp"
#include <systemlib/mavlink_log.h>
#include <matrix/math.hpp>

extern orb_advert_t mavlink_log_pub;

// required number of samples for sensor to initialize.
// This is a vision based position measurement so we assume
// as soon as we get one measurement it is initialized.
static const uint32_t		REQ_VISION_INIT_COUNT = 1;

// We don't want to deinitialize it because
// this will throw away a correction before it starts using the data so we
// set the timeout to 0.5 seconds
static const uint32_t		VISION_TIMEOUT = 500000;	// 0.5 s

// set pose/velocity as invalid if standard deviation is bigger than EP_MAX_STD_DEV
// TODO: the user should be allowed to set these values by a parameter
static constexpr float 	EP_MAX_STD_DEV = 100.0f;

void BlockLocalPositionEstimator::visionInit()
{
	// measure
	Vector<float, n_y_vision> y;

	if (visionMeasure(y) != OK) {
		_visionStats.reset();
		return;
	}

	// increament sums for mean
	if (_visionStats.getCount() > REQ_VISION_INIT_COUNT) {
		mavlink_and_console_log_info(&mavlink_log_pub, "[lpe] vision position init: "
					     "%5.2f %5.2f %5.2f m std %5.2f %5.2f %5.2f m",
					     double(_visionStats.getMean()(0)),
					     double(_visionStats.getMean()(1)),
					     double(_visionStats.getMean()(2)),
					     double(_visionStats.getStdDev()(0)),
					     double(_visionStats.getStdDev()(1)),
					     double(_visionStats.getStdDev()(2)));
		_sensorTimeout &= ~SENSOR_VISION;
		_sensorFault &= ~SENSOR_VISION;

		// get reference for global position
		globallocalconverter_getref(&_ref_lat, &_ref_lon, &_ref_alt);
		_global_ref_timestamp = _timeStamp;
		_is_global_cov_init = globallocalconverter_initialized();

		if (!_map_ref.init_done && _is_global_cov_init) {
			// initialize global origin using the visual estimator reference
			mavlink_and_console_log_info(&mavlink_log_pub, "[lpe] global origin init (vision) : lat %6.2f lon %6.2f alt %5.1f m",
						     double(_ref_lat), double(_ref_lon), double(_ref_alt));
			map_projection_init(&_map_ref, _ref_lat, _ref_lon);
			// set timestamp when origin was set to current time
			_time_origin = _timeStamp;
		}

		if (!_altOriginInitialized) {
			_altOriginInitialized = true;
			_altOriginGlobal = true;
			_altOrigin = globallocalconverter_initialized() ? _ref_alt : 0.0f;
		}
	}
}

int BlockLocalPositionEstimator::visionMeasure(Vector<float, n_y_vision> &y)
{
	uint8_t x_variance = _sub_visual_odom.get().COVARIANCE_MATRIX_X_VARIANCE;
	uint8_t y_variance = _sub_visual_odom.get().COVARIANCE_MATRIX_Y_VARIANCE;
	uint8_t z_variance = _sub_visual_odom.get().COVARIANCE_MATRIX_Z_VARIANCE;

	if (PX4_ISFINITE(_sub_visual_odom.get().pose_covariance[x_variance])) {
		// check if the vision data is valid based on the covariances
		_vision_eph = sqrtf(fmaxf(_sub_visual_odom.get().pose_covariance[x_variance],
					  _sub_visual_odom.get().pose_covariance[y_variance]));
		_vision_epv = sqrtf(_sub_visual_odom.get().pose_covariance[z_variance]);
		_vision_xy_valid = _vision_eph <= EP_MAX_STD_DEV;
		_vision_z_valid = _vision_epv <= EP_MAX_STD_DEV;

	} else {
		// if we don't have covariances, assume every reading
		_vision_xy_valid = true;
		_vision_z_valid = true;
	}

	if (!_vision_xy_valid || !_vision_z_valid) {
		_time_last_vision_p = _sub_visual_odom.get().timestamp;
		return -1;

	} else {
		_time_last_vision_p = _sub_visual_odom.get().timestamp;

		if (PX4_ISFINITE(_sub_visual_odom.get().x)) {
			y.setZero();
			y(Y_vision_x) = _sub_visual_odom.get().x;
			y(Y_vision_y) = _sub_visual_odom.get().y;
			y(Y_vision_z) = _sub_visual_odom.get().z;
			_visionStats.update(y);

			return OK;

		} else {
			return -1;
		}
	}
}

void BlockLocalPositionEstimator::visionCorrect()
{
	// measure
	Vector<float, n_y_vision> y;

	if (visionMeasure(y) != OK) {
		mavlink_and_console_log_info(&mavlink_log_pub, "[lpe] vision data invalid. eph: %f epv: %f", _vision_eph, _vision_epv);
		return;
	}

	// vision measurement matrix, measures position
	Matrix<float, n_y_vision, n_x> C;
	C.setZero();
	C(Y_vision_x, X_x) = 1;
	C(Y_vision_y, X_y) = 1;
	C(Y_vision_z, X_z) = 1;

	// noise matrix
	Matrix<float, n_y_vision, n_y_vision> R;
	R.setZero();

	// use std dev from vision data if available
	if (_vision_eph > _vision_xy_stddev.get()) {
		R(Y_vision_x, Y_vision_x) = _vision_eph * _vision_eph;
		R(Y_vision_y, Y_vision_y) = _vision_eph * _vision_eph;

	} else {
		R(Y_vision_x, Y_vision_x) = _vision_xy_stddev.get() * _vision_xy_stddev.get();
		R(Y_vision_y, Y_vision_y) = _vision_xy_stddev.get() * _vision_xy_stddev.get();
	}

	if (_vision_epv > _vision_z_stddev.get()) {
		R(Y_vision_z, Y_vision_z) = _vision_epv * _vision_epv;

	} else {
		R(Y_vision_z, Y_vision_z) = _vision_z_stddev.get() * _vision_z_stddev.get();
	}

	// vision delayed x
	uint8_t i_hist = 0;

	float vision_delay = (_timeStamp - _sub_visual_odom.get().timestamp) * 1e-6f;	// measurement delay in seconds

	if (vision_delay < 0.0f) { vision_delay = 0.0f; }

	// use auto-calculated delay from measurement if parameter is set to zero
	if (getDelayPeriods(_vision_delay.get() > 0.0f ? _vision_delay.get() : vision_delay, &i_hist) < 0) { return; }

	Vector<float, n_x> x0 = _xDelay.get(i_hist);

	// residual
	Matrix<float, n_y_vision, 1> r = y - C * x0;
	// residual covariance
	Matrix<float, n_y_vision, n_y_vision> S = C * _P * C.transpose() + R;

	// publish innovations
	for (size_t i = 0; i < 3; i++) {
		_pub_innov.get().vel_pos_innov[i] = r(i, 0);
		_pub_innov.get().vel_pos_innov_var[i] = S(i, i);
	}

	for (size_t i = 3; i < 6; i++) {
		_pub_innov.get().vel_pos_innov[i] = 0;
		_pub_innov.get().vel_pos_innov_var[i] = 1;
	}

	// residual covariance, (inverse)
	Matrix<float, n_y_vision, n_y_vision> S_I = inv<float, n_y_vision>(S);

	// fault detection
	float beta = (r.transpose() * (S_I * r))(0, 0);

	if (beta > BETA_TABLE[n_y_vision]) {
		if (!(_sensorFault & SENSOR_VISION)) {
			mavlink_and_console_log_info(&mavlink_log_pub, "[lpe] vision position fault, beta %5.2f", double(beta));
			_sensorFault |= SENSOR_VISION;
		}

	} else if (_sensorFault & SENSOR_VISION) {
		_sensorFault &= ~SENSOR_VISION;
		mavlink_and_console_log_info(&mavlink_log_pub, "[lpe] vision position OK");
	}

	// kalman filter correction if no fault
	if (!(_sensorFault & SENSOR_VISION)) {
		Matrix<float, n_x, n_y_vision> K = _P * C.transpose() * S_I;
		Vector<float, n_x> dx = K * r;
		_x += dx;
		_P -= K * C * _P;
	}
}

void BlockLocalPositionEstimator::visionCheckTimeout()
{
	if (_timeStamp - _time_last_vision_p > VISION_TIMEOUT) {
		if (!(_sensorTimeout & SENSOR_VISION)) {
			_sensorTimeout |= SENSOR_VISION;
			_visionStats.reset();
			mavlink_log_critical(&mavlink_log_pub, "[lpe] vision position timeout ");
		}
	}
}
