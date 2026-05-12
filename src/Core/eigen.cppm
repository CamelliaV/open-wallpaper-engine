module;

#include <Eigen/Dense>
#include <Eigen/Geometry>

export module eigen;

export namespace Eigen
{

// core templates
using Eigen::Array;
using Eigen::Matrix;
using Eigen::MatrixBase;
using Eigen::DenseBase;
using Eigen::PlainObjectBase;

// fixed-size float/double vectors
using Eigen::Vector2f;
using Eigen::Vector2d;
using Eigen::Vector2i;
using Eigen::Vector3f;
using Eigen::Vector3d;
using Eigen::Vector3i;
using Eigen::Vector4f;
using Eigen::Vector4d;
using Eigen::Vector4i;

// dynamic-size vectors
using Eigen::VectorXf;
using Eigen::VectorXd;
using Eigen::VectorXi;

// row vectors
using Eigen::RowVector2f;
using Eigen::RowVector2d;
using Eigen::RowVector3f;
using Eigen::RowVector3d;
using Eigen::RowVector4f;
using Eigen::RowVector4d;

// fixed-size matrices
using Eigen::Matrix2f;
using Eigen::Matrix2d;
using Eigen::Matrix3f;
using Eigen::Matrix3d;
using Eigen::Matrix4f;
using Eigen::Matrix4d;

// dynamic-size matrices
using Eigen::MatrixXf;
using Eigen::MatrixXd;
using Eigen::MatrixXi;

// geometry: rotations / transforms
using Eigen::Quaternion;
using Eigen::Quaternionf;
using Eigen::Quaterniond;
using Eigen::AngleAxis;
using Eigen::AngleAxisf;
using Eigen::AngleAxisd;
using Eigen::Rotation2D;
using Eigen::Rotation2Df;
using Eigen::Rotation2Dd;
using Eigen::Translation;
using Eigen::Translation2f;
using Eigen::Translation2d;
using Eigen::Translation3f;
using Eigen::Translation3d;
using Eigen::Scaling;
using Eigen::UniformScaling;

// affine / isometry / projective transforms
using Eigen::Transform;
using Eigen::Affine2f;
using Eigen::Affine2d;
using Eigen::Affine3f;
using Eigen::Affine3d;
using Eigen::Isometry2f;
using Eigen::Isometry2d;
using Eigen::Isometry3f;
using Eigen::Isometry3d;
using Eigen::Projective2f;
using Eigen::Projective2d;
using Eigen::Projective3f;
using Eigen::Projective3d;

// views / references
using Eigen::Map;
using Eigen::Ref;
using Eigen::Stride;
using Eigen::InnerStride;
using Eigen::OuterStride;

// enum tags (StorageOptions, AlignmentType, TransformTraits)
using Eigen::ColMajor;
using Eigen::RowMajor;
using Eigen::AutoAlign;
using Eigen::DontAlign;
using Eigen::Affine;
using Eigen::Isometry;
using Eigen::Projective;
using Eigen::Index;

} // namespace Eigen
