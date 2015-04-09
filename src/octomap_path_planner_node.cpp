#include <iostream>
#include <cmath>
#include <string>
#include <vector>
#include <queue>
#include <cstdlib>
#include <cassert>
#include <limits>

#include <boost/foreach.hpp>
#include <boost/thread.hpp>
#include <boost/chrono.hpp>
#include <boost/random.hpp>
#include <boost/random/uniform_real.hpp>
#include <boost/random/normal_distribution.hpp>

#include <ros/ros.h>
#include <std_msgs/Float32.h>
#include <geometry_msgs/Point.h>
#include <geometry_msgs/PoseArray.h>
#include <geometry_msgs/PoseStamped.h>
#include <geometry_msgs/Vector3.h>
#include <tf/transform_listener.h>
#include <sensor_msgs/PointCloud2.h>
#include <nav_msgs/Path.h>

#include <octomap/octomap.h>
#include <octomap_ros/conversions.h>
#include <octomap_msgs/Octomap.h>
#include <octomap_msgs/conversions.h>

#include <pcl/io/pcd_io.h>
#include <pcl/point_types.h>
#include <pcl/point_cloud.h>
#include <pcl/search/kdtree.h>
#include <pcl/features/normal_3d.h>
#include <pcl/features/boundary.h>
#include <pcl/segmentation/extract_clusters.h>
#include <pcl/filters/voxel_grid.h>
#include <pcl/filters/extract_indices.h>
#include <pcl/filters/passthrough.h>
#include <pcl/filters/radius_outlier_removal.h>

#include <pcl_conversions/pcl_conversions.h>

class OctomapPathPlanner
{
protected:
    ros::NodeHandle nh_;
    ros::NodeHandle pnh_;
    std::string frame_id_;
    std::string robot_frame_id_;
    ros::Subscriber octree_sub_;
    ros::Subscriber goal_point_sub_;
    ros::Subscriber goal_pose_sub_;
    ros::Publisher ground_pub_;
    ros::Publisher obstacles_pub_;
    ros::Publisher path_pub_;
    ros::Publisher twist_pub_;
    ros::Publisher target_pub_;
    ros::Publisher position_error_pub_;
    ros::Publisher orientation_error_pub_;
    tf::TransformListener tf_listener_;    
    geometry_msgs::PoseStamped robot_pose_;
    geometry_msgs::PoseStamped goal_;
    octomap::OcTree* octree_ptr_;
    pcl::PointCloud<pcl::PointXYZI> ground_pcl_;
    pcl::PointCloud<pcl::PointXYZ> obstacles_pcl_;
    pcl::octree::OctreePointCloudSearch<pcl::PointXYZI>::Ptr ground_octree_ptr_;
    pcl::octree::OctreePointCloudSearch<pcl::PointXYZ>::Ptr obstacles_octree_ptr_;
    ros::Timer controller_timer_;
    bool treat_unknown_as_free_;
    double robot_height_;
    double robot_radius_;
    double goal_reached_threshold_;
    double controller_frequency_;
    double local_target_radius_;
    double twist_linear_gain_;
    double twist_angular_gain_;
    double max_superable_height_;
    void startController();
    bool reached_position_;
public:
    OctomapPathPlanner();
    ~OctomapPathPlanner();
    void onOctomap(const octomap_msgs::Octomap::ConstPtr& msg);
    void onGoal(const geometry_msgs::PointStamped::ConstPtr& msg);
    void onGoal(const geometry_msgs::PoseStamped::ConstPtr& msg);
    void expandOcTree();
    bool isGround(const octomap::OcTreeKey& key);
    bool isObstacle(const octomap::OcTreeKey& key);
    template<class T> bool isNearObstacle(const T& point);
    void filterInflatedRegionFromGround();
    void computeGround();
    void projectGoalPositionToGround();
    void publishGroundCloud();
    void computeDistanceTransform();
    bool getRobotPose();
    double positionError();
    double orientationError();
    int generateTarget();
    bool generateLocalTarget(geometry_msgs::PointStamped& p_local);
    void generateTwistCommand(const geometry_msgs::PointStamped& local_target, geometry_msgs::Twist& twist);
    void controllerCallback(const ros::TimerEvent& event);
};


OctomapPathPlanner::OctomapPathPlanner()
    : pnh_("~"),
      frame_id_("/map"),
      robot_frame_id_("/base_link"),
      octree_ptr_(0L),
      treat_unknown_as_free_(false),
      robot_height_(0.5),
      robot_radius_(0.5),
      goal_reached_threshold_(0.2),
      controller_frequency_(2.0),
      local_target_radius_(0.4),
      twist_linear_gain_(0.5),
      twist_angular_gain_(1.0),
      max_superable_height_(0.2),
      reached_position_(false)
{
    pnh_.param("frame_id", frame_id_, frame_id_);
    pnh_.param("robot_frame_id", robot_frame_id_, robot_frame_id_);
    pnh_.param("treat_unknown_as_free", treat_unknown_as_free_, treat_unknown_as_free_);
    pnh_.param("robot_height", robot_height_, robot_height_);
    pnh_.param("robot_radius", robot_radius_, robot_radius_);
    pnh_.param("goal_reached_threshold", goal_reached_threshold_, goal_reached_threshold_);
    pnh_.param("controller_frequency", controller_frequency_, controller_frequency_);
    pnh_.param("local_target_radius", local_target_radius_, local_target_radius_);
    pnh_.param("twist_linear_gain", twist_linear_gain_, twist_linear_gain_);
    pnh_.param("twist_angular_gain", twist_angular_gain_, twist_angular_gain_);
    pnh_.param("max_superable_height", max_superable_height_, max_superable_height_);
    octree_sub_ = nh_.subscribe<octomap_msgs::Octomap>("octree_in", 1, &OctomapPathPlanner::onOctomap, this);
    goal_point_sub_ = nh_.subscribe<geometry_msgs::PointStamped>("goal_point_in", 1, &OctomapPathPlanner::onGoal, this);
    goal_pose_sub_ = nh_.subscribe<geometry_msgs::PoseStamped>("goal_pose_in", 1, &OctomapPathPlanner::onGoal, this);
    ground_pub_ = nh_.advertise<sensor_msgs::PointCloud2>("ground_cloud_out", 1, true);
    obstacles_pub_ = nh_.advertise<sensor_msgs::PointCloud2>("obstacles_cloud_out", 1, true);
    path_pub_ = nh_.advertise<nav_msgs::Path>("path_out", 1, true);
    twist_pub_ = nh_.advertise<geometry_msgs::Twist>("twist_out", 1, false);
    target_pub_ = nh_.advertise<geometry_msgs::PointStamped>("target_out", 1, false);
    position_error_pub_ = nh_.advertise<std_msgs::Float32>("position_error", 10, false);;
    orientation_error_pub_ = nh_.advertise<std_msgs::Float32>("orientation_error", 10, false);;
    ground_pcl_.header.frame_id = frame_id_;
    obstacles_pcl_.header.frame_id = frame_id_;
}


OctomapPathPlanner::~OctomapPathPlanner()
{
    if(octree_ptr_) delete octree_ptr_;
}


void OctomapPathPlanner::onOctomap(const octomap_msgs::Octomap::ConstPtr& msg)
{
    if(octree_ptr_) delete octree_ptr_;
    octree_ptr_ = octomap_msgs::binaryMsgToMap(*msg);

    expandOcTree();
    computeGround();
    computeDistanceTransform();
}


void OctomapPathPlanner::startController()
{
    controller_timer_ = nh_.createTimer(ros::Duration(1.0 / controller_frequency_), &OctomapPathPlanner::controllerCallback, this);
    reached_position_ = false;
}


void OctomapPathPlanner::onGoal(const geometry_msgs::PointStamped::ConstPtr& msg)
{
    try
    {
        geometry_msgs::PointStamped msg2;
        tf_listener_.transformPoint(frame_id_, *msg, msg2);
        goal_.header.stamp = msg2.header.stamp;
        goal_.header.frame_id = msg2.header.frame_id;
        goal_.pose.position.x = msg2.point.x;
        goal_.pose.position.y = msg2.point.y;
        goal_.pose.position.z = msg2.point.z;
        goal_.pose.orientation.x = 0.0;
        goal_.pose.orientation.y = 0.0;
        goal_.pose.orientation.z = 0.0;
        goal_.pose.orientation.w = 0.0;
        projectGoalPositionToGround();
        ROS_INFO("goal set to point (%f, %f, %f)",
            goal_.pose.position.x, goal_.pose.position.y, goal_.pose.position.z);

        startController();
    }
    catch(tf::TransformException& ex)
    {
        ROS_ERROR("Failed to lookup robot position: %s", ex.what());
    }

    computeDistanceTransform();
}


void OctomapPathPlanner::onGoal(const geometry_msgs::PoseStamped::ConstPtr& msg)
{
    try
    {
        tf_listener_.transformPose(frame_id_, *msg, goal_);
        projectGoalPositionToGround();
        ROS_INFO("goal set to pose (%f, %f, %f), (%f, %f, %f, %f)",
                goal_.pose.position.x, goal_.pose.position.y, goal_.pose.position.z,
                goal_.pose.orientation.x, goal_.pose.orientation.y, goal_.pose.orientation.z,
                goal_.pose.orientation.w);

        startController();
    }
    catch(tf::TransformException& ex)
    {
        ROS_ERROR("Failed to lookup robot position: %s", ex.what());
    }

    computeDistanceTransform();
}


void OctomapPathPlanner::expandOcTree()
{
    if(!octree_ptr_) return;

    unsigned int maxDepth = octree_ptr_->getTreeDepth();

    // expand collapsed occupied nodes until all occupied leaves are at maximum depth
    std::vector<octomap::OcTreeNode*> collapsed_occ_nodes;

    size_t initial_size = octree_ptr_->size();
    size_t num_rounds = 0;
    size_t expanded_nodes = 0;

    do
    {
        collapsed_occ_nodes.clear();
        for(octomap::OcTree::iterator it = octree_ptr_->begin(); it != octree_ptr_->end(); ++it)
        {
            if(octree_ptr_->isNodeOccupied(*it) && it.getDepth() < maxDepth)
            {
                collapsed_occ_nodes.push_back(&(*it));
            }
        }
        for(std::vector<octomap::OcTreeNode*>::iterator it = collapsed_occ_nodes.begin(); it != collapsed_occ_nodes.end(); ++it)
        {
            (*it)->expandNode();
        }

        expanded_nodes += collapsed_occ_nodes.size();
        num_rounds++;
    } while(collapsed_occ_nodes.size() > 0);

    //ROS_INFO("received octree of %ld nodes; expanded %ld nodes in %ld rounds.", initial_size, expanded_nodes, num_rounds);
}


bool OctomapPathPlanner::isGround(const octomap::OcTreeKey& key)
{
    octomap::OcTreeNode *node = octree_ptr_->search(key);
    if(!node) return false;
    if(!octree_ptr_->isNodeOccupied(node)) return false;

    double res = octree_ptr_->getResolution();
    int steps = ceil(robot_height_ / res);
    octomap::OcTreeKey key1(key);
    while(steps-- > 0)
    {
        key1[2]++;
        node = octree_ptr_->search(key1);
        if(!node)
        {
            if(!treat_unknown_as_free_) return false;
        }
        else
        {
            if(octree_ptr_->isNodeOccupied(node)) return false;
        }
    }
    return true;
}


bool OctomapPathPlanner::isObstacle(const octomap::OcTreeKey& key)
{
    octomap::OcTreeNode *node;
    double res = octree_ptr_->getResolution();
    int num_voxels = 1;

    // look up...
    octomap::OcTreeKey key1(key);
    while(true)
    {
        key1[2]++;
        node = octree_ptr_->search(key1);
        if(!node) break;
        if(node && !octree_ptr_->isNodeOccupied(node)) break;
        num_voxels++;
    }

    // look down...
    octomap::OcTreeKey key2(key);
    while(true)
    {
        key2[2]--;
        node = octree_ptr_->search(key2);
        if(!node) break;
        if(node && !octree_ptr_->isNodeOccupied(node)) break;
        num_voxels++;
    }

    return res * num_voxels > max_superable_height_;
}


template<class T>
bool OctomapPathPlanner::isNearObstacle(const T& point)
{
    std::vector<int> pointIdx2;
    std::vector<float> pointDistSq2;
    pcl::PointXYZ p;
    p.x = point.x;
    p.y = point.y;
    p.z = point.z;
    int num_points = obstacles_octree_ptr_->nearestKSearch(p, 1, pointIdx2, pointDistSq2);
    return num_points >= 1 && pointDistSq2[0] < (robot_radius_ * robot_radius_);
}


void OctomapPathPlanner::filterInflatedRegionFromGround()
{
    for(int i = 0; i < ground_pcl_.size(); )
    {
        if(isNearObstacle(ground_pcl_[i]))
        {
            std::swap(*(ground_pcl_.begin() + i), ground_pcl_.back());
            ground_pcl_.points.pop_back();
        }
        else i++;
    }
    ground_pcl_.width = ground_pcl_.points.size();
    ground_pcl_.height = 1;
}


void OctomapPathPlanner::computeGround()
{
    if(!octree_ptr_) return;

    ground_pcl_.clear();
    obstacles_pcl_.clear();

    for(octomap::OcTree::leaf_iterator it = octree_ptr_->begin(); it != octree_ptr_->end(); ++it)
    {
        if(!octree_ptr_->isNodeOccupied(*it)) continue;

        if(isGround(it.getKey()))
        {
            pcl::PointXYZI point;
            point.x = it.getX();
            point.y = it.getY();
            point.z = it.getZ();
            point.intensity = std::numeric_limits<float>::infinity();
            ground_pcl_.push_back(point);
        }
        else if(isObstacle(it.getKey()))
        {
            pcl::PointXYZ point;
            point.x = it.getX();
            point.y = it.getY();
            point.z = it.getZ();
            obstacles_pcl_.push_back(point);
        }
    }

    double res = octree_ptr_->getResolution();

    obstacles_octree_ptr_ = pcl::octree::OctreePointCloudSearch<pcl::PointXYZ>::Ptr(new pcl::octree::OctreePointCloudSearch<pcl::PointXYZ>(res));
    obstacles_octree_ptr_->setInputCloud(obstacles_pcl_.makeShared());
    obstacles_octree_ptr_->addPointsFromInputCloud();

    filterInflatedRegionFromGround();

    ground_octree_ptr_ = pcl::octree::OctreePointCloudSearch<pcl::PointXYZI>::Ptr(new pcl::octree::OctreePointCloudSearch<pcl::PointXYZI>(res));
    ground_octree_ptr_->setInputCloud(ground_pcl_.makeShared());
    ground_octree_ptr_->addPointsFromInputCloud();
}


void OctomapPathPlanner::projectGoalPositionToGround()
{
    pcl::PointXYZI goal;
    goal.x = goal_.pose.position.x;
    goal.y = goal_.pose.position.y;
    goal.z = goal_.pose.position.z;
    std::vector<int> pointIdx;
    std::vector<float> pointDistSq;
    if(ground_octree_ptr_->nearestKSearch(goal, 1, pointIdx, pointDistSq) < 1)
    {
        ROS_ERROR("Failed to project goal position to ground pcl");
        return;
    }
    int i = pointIdx[0];
    goal_.pose.position.x = ground_pcl_[i].x;
    goal_.pose.position.y = ground_pcl_[i].y;
    goal_.pose.position.z = ground_pcl_[i].z;
}


void OctomapPathPlanner::publishGroundCloud()
{
    if(ground_pub_.getNumSubscribers() > 0)
    {
        sensor_msgs::PointCloud2 msg;
        pcl::toROSMsg(ground_pcl_, msg);
        ground_pub_.publish(msg);
    }

    if(obstacles_pub_.getNumSubscribers() > 0)
    {
        sensor_msgs::PointCloud2 msg;
        pcl::toROSMsg(obstacles_pcl_, msg);
        obstacles_pub_.publish(msg);
    }
}


void OctomapPathPlanner::computeDistanceTransform()
{
    if(ground_pcl_.size() == 0)
    {
        ROS_INFO("skip computing distance transform because ground_pcl_ is empty");
        return;
    }

    // find goal index in ground pcl:
    std::vector<int> pointIdx;
    std::vector<float> pointDistSq;
    pcl::PointXYZI goal;
    goal.x = goal_.pose.position.x;
    goal.y = goal_.pose.position.y;
    goal.z = goal_.pose.position.z;
    if(ground_octree_ptr_->nearestKSearch(goal, 1, pointIdx, pointDistSq) < 1)
    {
        ROS_ERROR("unable to find goal in ground pcl");
        return;
    }

    double res = octree_ptr_->getResolution();
    int goal_idx = pointIdx[0];

    // distance to goal is zero (stored in the intensity channel):
    ground_pcl_[goal_idx].intensity = 0.0;

    std::queue<int> q;
    q.push(goal_idx);
    while(!q.empty())
    {
        int i = q.front();
        q.pop();

        // get neighbours:
        std::vector<int> pointIdx;
        std::vector<float> pointDistSq;
        ground_octree_ptr_->radiusSearch(ground_pcl_[i], 1.8 * res, pointIdx, pointDistSq);

        for(std::vector<int>::iterator it = pointIdx.begin(); it != pointIdx.end(); it++)
        {
            int j = *it;

            // intensity value are initially set to infinity.
            // if i is finite it means it has already been labeled.
            if(std::isfinite(ground_pcl_[j].intensity)) continue;

            // otherwise, label it:
            ground_pcl_[j].intensity = ground_pcl_[i].intensity + 1.0;

            // continue exploring neighbours:
            q.push(j);
        }
    }

    // normalize intensity:
    float imin = std::numeric_limits<float>::infinity();
    float imax = -std::numeric_limits<float>::infinity();
    for(pcl::PointCloud<pcl::PointXYZI>::iterator it = ground_pcl_.begin(); it != ground_pcl_.end(); ++it)
    {
        if(!std::isfinite(it->intensity)) continue;
        imin = fmin(imin, it->intensity);
        imax = fmax(imax, it->intensity);
    }
    const float eps = 0.01;
    float d = imax - imin + eps;
    for(pcl::PointCloud<pcl::PointXYZI>::iterator it = ground_pcl_.begin(); it != ground_pcl_.end(); ++it)
    {
        if(std::isfinite(it->intensity))
            it->intensity = (it->intensity - imin) / d;
        else
            it->intensity = 1.0;
    }

    publishGroundCloud();
}


bool OctomapPathPlanner::getRobotPose()
{
    try
    {
        geometry_msgs::PoseStamped robot_pose_local;
        robot_pose_local.header.frame_id = robot_frame_id_;
        robot_pose_local.pose.position.x = 0.0;
        robot_pose_local.pose.position.y = 0.0;
        robot_pose_local.pose.position.z = 0.0;
        robot_pose_local.pose.orientation.x = 0.0;
        robot_pose_local.pose.orientation.y = 0.0;
        robot_pose_local.pose.orientation.z = 0.0;
        robot_pose_local.pose.orientation.w = 1.0;
        tf_listener_.transformPose(frame_id_, robot_pose_local, robot_pose_);
        return true;
    }
    catch(tf::TransformException& ex)
    {
        ROS_ERROR("Failed to lookup robot position: %s", ex.what());
    }
}


double OctomapPathPlanner::positionError()
{
    return sqrt(
            pow(robot_pose_.pose.position.x - goal_.pose.position.x, 2) +
            pow(robot_pose_.pose.position.y - goal_.pose.position.y, 2) +
            pow(robot_pose_.pose.position.z - goal_.pose.position.z, 2)
    );
}


double OctomapPathPlanner::orientationError()
{
    // check if goal is only by position:
    double qnorm = pow(goal_.pose.orientation.w, 2) +
            pow(goal_.pose.orientation.x, 2) +
            pow(goal_.pose.orientation.y, 2) +
            pow(goal_.pose.orientation.z, 2);

    // if so, we never have position error:
    if(qnorm < 1e-5) return 0;

    // "Robotica - Modellistica Pianificazione e Controllo" eq. 3.88
    double nd = goal_.pose.orientation.w,
            ne = robot_pose_.pose.orientation.w;
    Eigen::Vector3d ed(goal_.pose.orientation.x, goal_.pose.orientation.y, goal_.pose.orientation.z),
            ee(robot_pose_.pose.orientation.x, robot_pose_.pose.orientation.y, robot_pose_.pose.orientation.z);
    Eigen::Vector3d eo = ne * ed - nd * ee - ed.cross(ee);
    return eo(2);
}


int OctomapPathPlanner::generateTarget()
{
    int best_index = -1;
    float best_value = std::numeric_limits<float>::infinity();

    pcl::PointXYZI robot_position;
    robot_position.x = robot_pose_.pose.position.x;
    robot_position.y = robot_pose_.pose.position.y;
    robot_position.z = robot_pose_.pose.position.z;

    std::vector<int> pointIdx;
    std::vector<float> pointDistSq;
    ground_octree_ptr_->radiusSearch(robot_position, local_target_radius_, pointIdx, pointDistSq);

    for(std::vector<int>::iterator it = pointIdx.begin(); it != pointIdx.end(); ++it)
    {
        pcl::PointXYZI& p = ground_pcl_[*it];

        if(best_index == -1 || p.intensity < best_value)
        {
            best_value = p.intensity;
            best_index = *it;
        }
    }

    return best_index;
}


bool OctomapPathPlanner::generateLocalTarget(geometry_msgs::PointStamped& p_local)
{
    int i = generateTarget();

    if(i == -1)
    {
        ROS_ERROR("Failed to find a target in robot vicinity");
        return false;
    }

    try
    {
        geometry_msgs::PointStamped p;
        p.header.frame_id = ground_pcl_.header.frame_id;
        p.point.x = ground_pcl_[i].x;
        p.point.y = ground_pcl_[i].y;
        p.point.z = ground_pcl_[i].z;
        tf_listener_.transformPoint(robot_frame_id_, p, p_local);
        target_pub_.publish(p);
        return true;
    }
    catch(tf::TransformException& ex)
    {
        ROS_ERROR("Failed to transform reference point: %s", ex.what());
        return false;
    }
}


void OctomapPathPlanner::generateTwistCommand(const geometry_msgs::PointStamped& local_target, geometry_msgs::Twist& twist)
{
    if(local_target.header.frame_id != robot_frame_id_)
    {
        ROS_ERROR("generateTwistCommand: local_target must be in frame '%s'", robot_frame_id_.c_str());
        return;
    }

    twist.linear.x = 0.0;
    twist.linear.y = 0.0;
    twist.linear.z = 0.0;
    twist.angular.x = 0.0;
    twist.angular.y = 0.0;
    twist.angular.z = 0.0;

    const geometry_msgs::Point& p = local_target.point;

    if(p.x < 0 || fabs(p.y) > p.x)
    {
        // turn in place
        twist.angular.z = (p.y > 0 ? 1 : -1) * twist_angular_gain_;
    }
    else
    {
        // make arc
        double center_y = (pow(p.x, 2) + pow(p.y, 2)) / (2 * p.y);
        double theta = fabs(atan2(p.x, fabs(center_y) - fabs(p.y)));
        double arc_length = fabs(center_y * theta);

        twist.linear.x = twist_linear_gain_ * arc_length;
        twist.angular.z = twist_angular_gain_ * (p.y >= 0 ? 1 : -1) * theta;
    }
}


void OctomapPathPlanner::controllerCallback(const ros::TimerEvent& event)
{
    if(!getRobotPose())
    {
        ROS_ERROR("controllerCallback: failed to get robot pose");
        return;
    }

    geometry_msgs::Twist twist;
    twist.linear.x = 0.0;
    twist.linear.y = 0.0;
    twist.linear.z = 0.0;
    twist.angular.x = 0.0;
    twist.angular.y = 0.0;
    twist.angular.z = 0.0;

    std_msgs::Float32 ep, eo;

    ep.data = positionError();
    position_error_pub_.publish(ep);

    eo.data = orientationError();
    orientation_error_pub_.publish(eo);

    const char *status_str;

    if((!reached_position_ && ep.data > goal_reached_threshold_)
        || (reached_position_ && ep.data > 2 * goal_reached_threshold_))
    {
        // regulate position

        status_str = "REGULATING POSITION";

        reached_position_ = false;

        geometry_msgs::PointStamped local_target;

        if(!generateLocalTarget(local_target))
        {
            ROS_ERROR("controllerCallback: failed to generate a local target to follow");
            return;
        }

        generateTwistCommand(local_target, twist);
    }
    else
    {
        reached_position_ = true;

        if(fabs(eo.data) > 0.02)
        {
            // regulate orientation

            status_str = "REGULATING ORIENTATION";

            twist.angular.z = twist_angular_gain_ * eo.data;
        }
        else
        {
            // goal reached

            status_str = "REACHED GOAL";

            ROS_INFO("goal reached! stopping controller timer");

            controller_timer_.stop();
        }
    }

    ROS_INFO("controller: ep=%f, eo=%f, status=%s", ep.data, eo.data, status_str);

    twist_pub_.publish(twist);
}


int main(int argc, char **argv)
{
    ros::init(argc, argv, "octomap_path_planner");

    OctomapPathPlanner p;
    ros::spin();

    return 0;
}

