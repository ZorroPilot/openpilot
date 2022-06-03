#include <cstring>

#include "libyuv.h"

#include "common/mat.h"
#include "common/modeldata.h"
#include "common/params.h"
#include "common/timing.h"
#include "selfdrive/hardware/hw.h"

#include "selfdrive/modeld/models/dmonitoring.h"

constexpr int MODEL_WIDTH = 1440;
constexpr int MODEL_HEIGHT = 960;

template <class T>
static inline T *get_buffer(std::vector<T> &buf, const size_t size) {
  if (buf.size() < size) buf.resize(size);
  return buf.data();
}

void dmonitoring_init(DMonitoringModelState* s) {
  s->is_rhd = Params().getBool("IsRHD");

#ifdef USE_ONNX_MODEL
  s->m = new ONNXModel("models/dmonitoring_model.onnx", &s->output[0], OUTPUT_SIZE, USE_DSP_RUNTIME);
#else
  s->m = new SNPEModel("models/dmonitoring_model_q.dlc", &s->output[0], OUTPUT_SIZE, USE_DSP_RUNTIME);
#endif

  s->m->addCalib(s->calib, CALIB_LEN);
}

void parse_driver_data(DriverStateResult &ds_res, const DMonitoringModelState* s, int out_idx_offset) {
  for (int i = 0; i < 3; ++i) {
    ds_res.face_orientation[i] = s->output[out_idx_offset+i] * REG_SCALE;
    ds_res.face_orientation_std[i] = exp(s->output[out_idx_offset+6+i]);
  }
  for (int i = 0; i < 2; ++i) {
    ds_res.face_position[i] = s->output[out_idx_offset+3+i] * REG_SCALE;
    ds_res.face_position_std[i] = exp(s->output[out_idx_offset+9+i]);
  }
  for (int i = 0; i < 4; ++i) {
    ds_res.ready_prob[i] = sigmoid(s->output[out_idx_offset+35+i]);
  }
  for (int i = 0; i < 2; ++i) {
    ds_res.not_ready_prob[i] = sigmoid(s->output[out_idx_offset+39+i]);
  }
  ds_res.face_prob = sigmoid(s->output[out_idx_offset+12]);
  ds_res.left_eye_prob = sigmoid(s->output[out_idx_offset+21]);
  ds_res.right_eye_prob = sigmoid(s->output[out_idx_offset+30]);
  ds_res.left_blink_prob = sigmoid(s->output[out_idx_offset+31]);
  ds_res.right_blink_prob = sigmoid(s->output[out_idx_offset+32]);
  ds_res.sunglasses_prob = sigmoid(s->output[out_idx_offset+33]);
  ds_res.occluded_prob = sigmoid(s->output[out_idx_offset+34]);
}

void fill_driver_data(cereal::DriverState::DriverData::Builder ddata, const DriverStateResult &ds_res) {
  ddata.setFaceOrientation(ds_res.face_orientation);
  ddata.setFaceOrientationStd(ds_res.face_orientation_std);
  ddata.setFacePosition(ds_res.face_position);
  ddata.setFacePositionStd(ds_res.face_position_std);
  ddata.setFaceProb(ds_res.face_prob);
  ddata.setLeftEyeProb(ds_res.left_eye_prob);
  ddata.setRightEyeProb(ds_res.right_eye_prob);
  ddata.setLeftBlinkProb(ds_res.left_blink_prob);
  ddata.setRightBlinkProb(ds_res.right_blink_prob);
  ddata.setSunglassesProb(ds_res.sunglasses_prob);
  ddata.setOccludedProb(ds_res.occluded_prob);
  ddata.setReadyProb(ds_res.ready_prob);
  ddata.setNotReadyProb(ds_res.not_ready_prob);
}

DMonitoringModelResult dmonitoring_eval_frame(DMonitoringModelState* s, void* stream_buf, int width, int height, int stride, int uv_offset, float *calib) {
  int v_off = height - MODEL_HEIGHT;
  int h_off = (width - MODEL_WIDTH) / 2;
  int yuv_buf_len = MODEL_WIDTH * MODEL_HEIGHT;

  uint8_t *raw_buf = (uint8_t *) stream_buf;
  // vertical crop free
  uint8_t *raw_y_start = raw_buf + stride * v_off;

  float *net_input_buf = get_buffer(s->net_input_buf, yuv_buf_len);

  // snpe UserBufferEncodingUnsigned8Bit doesn't work
  // fast float conversion instead, also does h crop and scales to 0-1
  for (int r = 0; r < MODEL_HEIGHT; ++r) {
    libyuv::ByteToFloat(raw_y_start + r * stride + h_off, net_input_buf + r * MODEL_WIDTH, 0.003921569f, MODEL_WIDTH);
  }

  // printf("preprocess completed. %d \n", yuv_buf_len);
  // FILE *dump_yuv_file = fopen("/tmp/rawdump.yuv", "wb");
  // fwrite(net_input_buf, yuv_buf_len, sizeof(float), dump_yuv_file);
  // fclose(dump_yuv_file);

  // # testing:
  // dat = np.fromfile('/tmp/rawdump.yuv', dtype=np.float32)
  // dat = dat.reshape(1,6,320,512) * 128. + 128.
  // frame = tensor_to_frames(dat)[0]
  // frame = cv2.cvtColor(frame, cv2.COLOR_YUV2RGB_I420)

  double t1 = millis_since_boot();
  s->m->addImage(net_input_buf, yuv_buf_len);
  for (int i = 0; i < CALIB_LEN; i++) {
    s->calib[i] = calib[i];
  }
  s->m->execute();
  double t2 = millis_since_boot();

  DMonitoringModelResult model_res = {0};
  parse_driver_data(model_res.driver_state_lhd, s, 0);
  parse_driver_data(model_res.driver_state_rhd, s, 41);
  model_res.poor_vision = sigmoid(s->output[82]);
  model_res.wheel_on_right = sigmoid(s->output[83]);
  model_res.dsp_execution_time = (t2 - t1) / 1000.;

  return model_res;
}

void dmonitoring_publish(PubMaster &pm, uint32_t frame_id, const DMonitoringModelResult &model_res, float execution_time, kj::ArrayPtr<const float> raw_pred) {
  // make msg
  MessageBuilder msg;
  auto framed = msg.initEvent().initDriverState();
  framed.setFrameId(frame_id);
  framed.setModelExecutionTime(execution_time);
  framed.setDspExecutionTime(model_res.dsp_execution_time);

  framed.setPoorVision(model_res.poor_vision);
  framed.setWheelOnRight(model_res.wheel_on_right);
  fill_driver_data(framed.initDriverDataLH(), model_res.driver_state_lhd);
  fill_driver_data(framed.initDriverDataRH(), model_res.driver_state_rhd);

  if (send_raw_pred) {
    framed.setRawPredictions(raw_pred.asBytes());
  }

  pm.send("driverState", msg);
}

void dmonitoring_free(DMonitoringModelState* s) {
  delete s->m;
}
