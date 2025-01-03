#include <cmath>
#include <math.h>
#include <deque>
#include <mutex>
#include <thread>
#include <fstream>
#include <csignal>
#include <ros/ros.h>
#include <so3_math.h>
#include <Eigen/Eigen>
#include <common_lib.h>
#include <pcl/common/io.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <condition_variable>
#include <nav_msgs/Odometry.h>
#include <pcl/common/transforms.h>
#include <pcl/kdtree/kdtree_flann.h>
#include <tf/transform_broadcaster.h>
#include <eigen_conversions/eigen_msg.h>
#include <pcl_conversions/pcl_conversions.h>
#include <sensor_msgs/Imu.h>
#include <sensor_msgs/PointCloud2.h>
#include <geometry_msgs/Vector3.h>
#include "use-ikfom.hpp"

/// *************Preconfiguration

const bool time_list(PointType &x, PointType &y) {return (x.curvature < y.curvature);};

/// *************IMU Process and undistortion
class ImuProcess
{
 public:
  EIGEN_MAKE_ALIGNED_OPERATOR_NEW

  ImuProcess();
  ~ImuProcess();
  
  void Reset();
  void Reset(double start_timestamp, const sensor_msgs::ImuConstPtr &lastimu);
  void set_extrinsic(const V3D &transl, const M3D &rot);
  void set_extrinsic(const V3D &transl);
  void set_extrinsic(const MD(4,4) &T);
  void set_gyr_cov(const V3D &scaler);
  void set_acc_cov(const V3D &scaler);
  void set_gyr_bias_cov(const V3D &b_g);
  void set_acc_bias_cov(const V3D &b_a);
  Eigen::Matrix<double, 12, 12> Q;
  void Process(const MeasureGroup &meas,  esekfom::esekf<state_ikfom, 12, input_ikfom> &kf_state, PointCloudXYZI::Ptr pcl_un_);

  // ofstream fout_imu;
  V3D cov_acc;
  V3D cov_gyr;
  V3D cov_acc_scale;
  V3D cov_gyr_scale;
  V3D cov_bias_gyr;
  V3D cov_bias_acc;
  double first_lidar_time;
  double start_timestamp_;  /// 开始时间戳
  V3D delta_pos;
  double gnss_time, delta_pos_time;
  int IMU_INIT_COUNT = 0;
  int imu_skip = 0, init_cnt=0;
  
 private:
  void IMU_init(const MeasureGroup &meas, esekfom::esekf<state_ikfom, 12, input_ikfom> &kf_state, int &N);
  void UndistortPcl(const MeasureGroup &meas, esekfom::esekf<state_ikfom, 12, input_ikfom> &kf_state, PointCloudXYZI &pcl_in_out);

  PointCloudXYZI::Ptr cur_pcl_un_;
  sensor_msgs::ImuConstPtr last_imu_;
  deque<sensor_msgs::ImuConstPtr> v_imu_;
  vector<Pose6D> IMUpose;
  vector<M3D>    v_rot_pcl_;
  M3D Lidar_R_wrt_IMU;
  V3D Lidar_T_wrt_IMU;
  V3D mean_acc;
  V3D mean_gyr;
  V3D angvel_last;
  V3D acc_s_last;
  double mean_acc_norm;
  double last_lidar_end_time_;
  int    init_iter_num = 1;
  bool   b_first_frame_ = true;
  bool   imu_need_init_ = true;
};

ImuProcess::ImuProcess()
    : b_first_frame_(true), imu_need_init_(true), start_timestamp_(-1)
{
  init_iter_num = 1;
  Q = process_noise_cov();
  cov_acc       = V3D(0.1, 0.1, 0.1);
  cov_gyr       = V3D(0.01, 0.01, 0.01);
  cov_bias_gyr  = V3D(0.0001, 0.0001, 0.0001);
  cov_bias_acc  = V3D(0.0001, 0.0001, 0.0001);
  mean_acc      = V3D(0, 0, -1.0);
  mean_gyr      = V3D(0, 0, 0);
  mean_acc_norm = mean_acc.norm();
  angvel_last     = Zero3d;
  Lidar_T_wrt_IMU = Zero3d;
  Lidar_R_wrt_IMU = Eye3d;
  last_imu_.reset(new sensor_msgs::Imu());
}

ImuProcess::~ImuProcess() {}

void ImuProcess::Reset() 
{
  // ROS_WARN("Reset ImuProcess");
  mean_acc      = V3D(0, 0, -1.0);
  mean_gyr      = V3D(0, 0, 0);
  mean_acc_norm = mean_acc.norm();
  angvel_last       = Zero3d;
  imu_need_init_    = true;
  start_timestamp_  = -1;
  init_iter_num     = 1;
  v_imu_.clear();
  IMUpose.clear();
  last_imu_.reset(new sensor_msgs::Imu());
  cur_pcl_un_.reset(new PointCloudXYZI());
}

void ImuProcess::set_extrinsic(const MD(4,4) &T)
{
  Lidar_T_wrt_IMU = T.block<3,1>(0,3);
  Lidar_R_wrt_IMU = T.block<3,3>(0,0);
}

void ImuProcess::set_extrinsic(const V3D &transl)
{
  Lidar_T_wrt_IMU = transl;
  Lidar_R_wrt_IMU.setIdentity();
}

void ImuProcess::set_extrinsic(const V3D &transl, const M3D &rot)
{
  Lidar_T_wrt_IMU = transl;
  Lidar_R_wrt_IMU = rot;
}

void ImuProcess::set_gyr_cov(const V3D &scaler)
{
  cov_gyr_scale = scaler;
}

void ImuProcess::set_acc_cov(const V3D &scaler)
{
  cov_acc_scale = scaler;
}

void ImuProcess::set_gyr_bias_cov(const V3D &b_g)
{
  cov_bias_gyr = b_g;
}

void ImuProcess::set_acc_bias_cov(const V3D &b_a)
{
  cov_bias_acc = b_a;
}

void ImuProcess::IMU_init(const MeasureGroup &meas, esekfom::esekf<state_ikfom, 12, input_ikfom> &kf_state, int &N)
{
  /** 1. initializing the gravity, gyro bias, acc and gyro covariance
   ** 2. normalize the acceleration measurenments to unit gravity **/
  
  V3D cur_acc, cur_gyr, acc_delta, gyr_delta;
  double N1_INV, N2;
  if (b_first_frame_)
  {
    Reset();
    N = 1;
    b_first_frame_ = false;
    const auto &imu_acc = meas.imu.front()->linear_acceleration;
    const auto &gyr_acc = meas.imu.front()->angular_velocity;
    mean_acc << imu_acc.x, imu_acc.y, imu_acc.z;
    mean_gyr << gyr_acc.x, gyr_acc.y, gyr_acc.z;
	first_lidar_time = meas.lidar_beg_time;
    cov_acc << 0, 0, 0;
    cov_gyr << 0, 0, 0;
  }

  for (const auto &imu : meas.imu)
  {
    const auto &imu_acc = imu->linear_acceleration;
    const auto &gyr_acc = imu->angular_velocity;
    cur_acc << imu_acc.x, imu_acc.y, imu_acc.z;
    cur_gyr << gyr_acc.x, gyr_acc.y, gyr_acc.z;

    acc_delta = cur_acc - mean_acc;
    gyr_delta = cur_gyr - mean_gyr;
    mean_acc += acc_delta / N;
    mean_gyr += gyr_delta / N;
    N1_INV = 1.0 / N;
    N2 = 1 - N1_INV;
    cov_acc = cov_acc * N2 + acc_delta.cwiseProduct(acc_delta) * N1_INV;
    cov_gyr = cov_gyr * N2 + gyr_delta.cwiseProduct(gyr_delta) * N1_INV;
    N ++;
  }
}

void ImuProcess::UndistortPcl(const MeasureGroup &meas, esekfom::esekf<state_ikfom, 12, input_ikfom> &kf_state, PointCloudXYZI &pcl_out)
{
  /*** add the imu of the last frame-tail to the of current frame-head ***/
  auto v_imu = meas.imu;
  v_imu.push_front(last_imu_);
  const double &imu_beg_time = v_imu.front()->header.stamp.toSec();
  const double &imu_end_time = v_imu.back()->header.stamp.toSec();
  const double &pcl_beg_time = meas.lidar_beg_time;
  const double &pcl_end_time = meas.lidar_end_time;
  
  /*** sort point clouds by offset time ***/
  pcl_out = *(meas.lidar);
  sort(pcl_out.points.begin(), pcl_out.points.end(), time_list);

  /*** Initialize IMU pose ***/
  state_ikfom imu_state = kf_state.get_x();
  IMUpose.clear();
  IMUpose.push_back(set_pose6d(0.0, acc_s_last, angvel_last, imu_state.vel, imu_state.pos, imu_state.rot.toRotationMatrix()));

  /*** forward propagation at each imu point ***/
  V3D angvel_avr, acc_avr, acc_imu, vel_imu, pos_imu;
  M3D R_imu;

  double dt = 0;
  Q.block<3, 3>(0, 0).diagonal() = cov_gyr;
  Q.block<3, 3>(3, 3).diagonal() = cov_acc;
  Q.block<3, 3>(6, 6).diagonal() = cov_bias_gyr;
  Q.block<3, 3>(9, 9).diagonal() = cov_bias_acc;

  input_ikfom in;
  for (auto it_imu = v_imu.begin(); it_imu < (v_imu.end() - 1); it_imu++)
  {
    auto &&head = *(it_imu);
    auto &&tail = *(it_imu + 1);
    
    if (tail->header.stamp.toSec() < last_lidar_end_time_)    continue;
    
    angvel_avr<<0.5 * (head->angular_velocity.x + tail->angular_velocity.x),
                0.5 * (head->angular_velocity.y + tail->angular_velocity.y),
                0.5 * (head->angular_velocity.z + tail->angular_velocity.z);
    acc_avr   <<0.5 * (head->linear_acceleration.x + tail->linear_acceleration.x),
                0.5 * (head->linear_acceleration.y + tail->linear_acceleration.y),
                0.5 * (head->linear_acceleration.z + tail->linear_acceleration.z);

    acc_avr     = acc_avr * G_m_s2 / mean_acc_norm; // - state_inout.ba;

    if(head->header.stamp.toSec() < last_lidar_end_time_) {
      dt = tail->header.stamp.toSec() - last_lidar_end_time_;
    }
    else {
      dt = tail->header.stamp.toSec() - head->header.stamp.toSec();
    }
    
    in.acc = acc_avr;
    in.gyro = angvel_avr;
    kf_state.predict(dt, Q, in);

    /* save the poses at each IMU measurements */
    imu_state = kf_state.get_x();
    
    angvel_last = angvel_avr - imu_state.bg;
    acc_s_last  = imu_state.rot * (acc_avr - imu_state.ba);
    for(int i=0; i<3; i++) {
      acc_s_last[i] += imu_state.grav[i];
    }
    double &&offs_t = tail->header.stamp.toSec() - pcl_beg_time;
    IMUpose.push_back(set_pose6d(offs_t, acc_s_last, angvel_last, imu_state.vel, imu_state.pos, imu_state.rot.toRotationMatrix()));
  }

  /*** calculated the pos and attitude prediction at the frame-end ***/
  {
    angvel_avr << v_imu.back()->angular_velocity.x,
                  v_imu.back()->angular_velocity.y,
                  v_imu.back()->angular_velocity.z;
    acc_avr    << v_imu.back()->linear_acceleration.x,
                  v_imu.back()->linear_acceleration.y,
                  v_imu.back()->linear_acceleration.z;
    in.acc = acc_avr;
    in.gyro = angvel_avr;
    dt = pcl_end_time - imu_end_time;
    if (dt < 0)
      ROS_ERROR("Motion compensation IMU time is later than Lidar scan end time!");

    kf_state.predict(dt, Q, in);
    
    imu_state = kf_state.get_x();   //更新IMU状态，以便于下一帧使用
    last_imu_ = meas.imu.back();    ////保存最后一个IMU测量，以便于下一帧使用
    last_lidar_end_time_ = pcl_end_time;  /////保存这一帧最后一个雷达测量的结束时间，以便于下一帧使用
  }

  /*** undistort each lidar point (backward propagation) ***/
  if (pcl_out.points.begin() == pcl_out.points.end())
    return;
  auto it_pcl = pcl_out.points.end() - 1;
  bool flag_pcl_end = false;
  for (auto head = prev(IMUpose.end()); head != IMUpose.begin(); head--)
  {
    R_imu<<MAT_FROM_ARRAY(head->rot);
    // cout<<"head imu acc: "<<acc_imu.transpose()<<endl;
    vel_imu<<VEC_FROM_ARRAY(head->vel);
    pos_imu<<VEC_FROM_ARRAY(head->pos);
    acc_imu<<VEC_FROM_ARRAY(head->acc);
    angvel_avr<<VEC_FROM_ARRAY(head->gyr);

    if (flag_pcl_end) {
      continue;
    }
    
    for(; it_pcl->curvature > head->offset_time; it_pcl --)
    {
      dt = it_pcl->curvature - head->offset_time;

      /* Transform to the 'end' frame, using only the rotation
       * Note: Compensation direction is INVERSE of Frame's moving direction
       * So if we want to compensate a point at timestamp-i to the frame-e
       * P_compensate = R_imu_e ^ T * (R_i * P_i + T_ei) where T_ei is represented in global frame */
      M3D R_i(R_imu * Exp(angvel_avr, dt));
      
      V3D P_i(it_pcl->x, it_pcl->y, it_pcl->z);
      V3D T_ei(pos_imu + vel_imu * dt + 0.5 * acc_imu * dt * dt - imu_state.pos);
      V3D P_compensate = imu_state.offset_R_L_I.conjugate() * (imu_state.rot.conjugate() * (R_i * (imu_state.offset_R_L_I * P_i + imu_state.offset_T_L_I) + T_ei) - imu_state.offset_T_L_I);// not accurate!
      
      // save Undistorted points and their rotation
      it_pcl->x = P_compensate(0);
      it_pcl->y = P_compensate(1);
      it_pcl->z = P_compensate(2);

      if (it_pcl == pcl_out.points.begin()) {
        flag_pcl_end = true;
        break;
      }
    }
  }
}

void ImuProcess::Process(const MeasureGroup &meas,  esekfom::esekf<state_ikfom, 12, input_ikfom> &kf_state, PointCloudXYZI::Ptr cur_pcl_un_)
{
  if(meas.imu.empty()) {
    ROS_WARN("No imu data in preprocess!");
    if(meas.lidar != nullptr)
      cout << "lidar time: " << meas.lidar_beg_time-first_lidar_time << ", " << meas.lidar_end_time-first_lidar_time << "s" << endl;
    return;
  }
  ROS_ASSERT(meas.lidar != nullptr);

  if (imu_need_init_)
  {
    /// The very first lidar frame
    IMU_init(meas, kf_state, init_iter_num);
    imu_need_init_ = true;
    if (init_iter_num > IMU_INIT_COUNT)
    {
      imu_need_init_ = false;
      mean_acc_norm = mean_acc.norm();
      cov_acc *= G_m_s2*G_m_s2 / (mean_acc_norm*mean_acc_norm);
      for (int i=0; i<3; i++) {
        cov_acc[i] = max(cov_acc[i], cov_acc_scale[i]);
        cov_gyr[i] = max(cov_gyr[i], cov_gyr_scale[i]);
      }

      state_ikfom init_state = kf_state.get_x();
      init_state.grav = S2(-mean_acc / mean_acc_norm * G_m_s2);
      init_state.bg  = mean_gyr;
	  init_state.offset_T_L_I = Lidar_T_wrt_IMU;  ///lidar到IMU外参：平移矩阵
      init_state.offset_R_L_I = Lidar_R_wrt_IMU;  ///lidar到IMU外参：旋转矩阵
	
      kf_state.change_x(init_state);

      esekfom::esekf<state_ikfom, 12, input_ikfom>::cov init_P = kf_state.get_P();
      init_P.setIdentity();
      init_P(6,6) = init_P(7,7) = init_P(8,8) = 1e-5;
      init_P(9,9) = init_P(10,10) = init_P(11,11) = 1e-5;
      for (int i=0; i<3; ++i) {
        init_P(15+i,15+i) = cov_bias_gyr[i];
        init_P(18+i,18+i) = cov_bias_acc[i];
      }
      init_P(21,21) = init_P(22,22) = 1e-5; 
      kf_state.change_P(init_P);

      last_imu_ = meas.imu.back();

      std::cout << endl << "IMU time: "<< last_imu_->header.stamp.toSec()-first_lidar_time << "s, IMU Initial Done! " << endl;
      std::cout << fixed << setprecision(5) 
                << "gravity: " << init_state.grav << ", " << G_m_s2 << " m/s2" << endl
                << "mean acc: " << -mean_acc.transpose() << ", " << mean_acc_norm << " g" << endl
                << "gyr bias: " << init_state.bg << endl 
                << "acc bias: " << init_state.ba << endl
                << "cov of bg: " << cov_bias_gyr.transpose() << endl 
                << "cov of ba: " << cov_bias_acc.transpose() << endl
                << "cov of gyr: " << cov_gyr.transpose() << endl
                << "cov of acc: " << cov_acc.transpose() << endl << endl;
    }

    return;
  }

  UndistortPcl(meas, kf_state, *cur_pcl_un_);
}
