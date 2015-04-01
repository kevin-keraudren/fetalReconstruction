/*=========================================================================
Library   : Image Registration Toolkit (IRTK)
Copyright : Imperial College, Department of Computing
Visual Information Processing (VIP), 2011 onwards
Date      : $Date: 2013-11-15 14:36:30 +0100 (Fri, 15 Nov 2013) $
Version   : $Revision: 1 $
Changes   : $Author: bkainz $

Copyright (c) 2014, Bernhard Kainz, Markus Steinberger,
Maria Murgasova, Kevin Keraudren
All rights reserved.

If you use this work for research we would very much appreciate if you cite
Bernhard Kainz, Markus Steinberger, Maria Kuklisova-Murgasova, Christina Malamateniou,
Wolfgang Wein, Thomas Torsney-Weir, Torsten Moeller, Mary Rutherford,
Joseph V. Hajnal and Daniel Rueckert:
Fast Volume Reconstruction from Motion Corrupted 2D Slices.
IEEE Transactions on Medical Imaging, in press, 2015

IRTK IS PROVIDED UNDER THE TERMS OF THIS CREATIVE
COMMONS PUBLIC LICENSE ("CCPL" OR "LICENSE"). THE WORK IS PROTECTED BY
COPYRIGHT AND/OR OTHER APPLICABLE LAW. ANY USE OF THE WORK OTHER THAN
AS AUTHORIZED UNDER THIS LICENSE OR COPYRIGHT LAW IS PROHIBITED.

BY EXERCISING ANY RIGHTS TO THE WORK PROVIDED HERE, YOU ACCEPT AND AGREE
TO BE BOUND BY THE TERMS OF THIS LICENSE. TO THE EXTENT THIS LICENSE MAY BE
CONSIDERED TO BE A CONTRACT, THE LICENSOR GRANTS YOU THE RIGHTS CONTAINED
HERE IN CONSIDERATION OF YOUR ACCEPTANCE OF SUCH TERMS AND CONDITIONS.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation
and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
=========================================================================*/

#define NOMINMAX
#define _USE_MATH_DEFINES

#include <irtkReconstructionGPU.h>
#include <irtkResampling.h>
#include <irtkRegistration.h>
#include <irtkImageRigidRegistration.h>
#include <irtkImageRigidRegistrationWithPadding.h>
#include <irtkImageFunction.h>
#include <irtkTransformation.h>
//#include <irtkMeanShift.h>
//#include <irtkCRF.h>
#include <math.h>
#include <stdlib.h>

#include <boost/filesystem.hpp>
using namespace boost::filesystem;

#if HAVE_CULA
#include <cula.h>
#endif

/* Auxiliary functions (not reconstruction specific) */

void headertool_dofin( irtkGreyImage &image,
                       irtkRigidTransformation &transformation ) {

    irtkImageAttributes attr = image.GetImageAttributes();

    // Origin:
    transformation.Transform( attr._xorigin,
                               attr._yorigin,
                               attr._zorigin );

    // Grid spacings:
    irtkVector v(3);
    irtkVector u(3);

    // Zero vector.
    u.Initialize(3);
    transformation.Transform(u(0), u(1), u(2));
    
    for (int i = 0; i < 3; i++)
        v(i) = attr._xaxis[i];
    
    transformation.Transform(v(0), v(1), v(2));
    v = v - u;
    attr._dx = attr._dx * v.Norm();
    
    for (int i = 0; i < 3; i++)
        v(i) = attr._yaxis[i];
		
    transformation.Transform(v(0), v(1), v(2));
    v = v - u;
    attr._dy = attr._dy * v.Norm();

    for (int i = 0; i < 3; i++)
        v(i) = attr._zaxis[i];
    
    transformation.Transform(v(0), v(1), v(2));
    v = v - u;
    attr._dz = attr._dz * v.Norm();

    // Axes:
    // Isolate rotation part of transformation.
    irtkRigidTransformation rotation;
    for (int i = 3; i < 6; i++)
        rotation.Put(i, transformation.Get(i));

    rotation.Transform(attr._xaxis[0], attr._xaxis[1], attr._xaxis[2]);
    rotation.Transform(attr._yaxis[0], attr._yaxis[1], attr._yaxis[2]);
    rotation.Transform(attr._zaxis[0], attr._zaxis[1], attr._zaxis[2]);

    // Grid size:
    // Remains the same so no need to do anything.

    // Update image attributes
    image.PutOrientation(attr._xaxis,attr._yaxis,attr._zaxis);
    image.PutOrigin(attr._xorigin,attr._yorigin,attr._zorigin);
    image.PutPixelSize(attr._dx,attr._dy,attr._dz);
}

void bbox(irtkRealImage &stack,
  irtkRigidTransformation &transformation,
  double &min_x,
  double &min_y,
  double &min_z,
  double &max_x,
  double &max_y,
  double &max_z) {

  cout << "bbox" << endl;

  min_x = voxel_limits<irtkRealPixel>::max();
  min_y = voxel_limits<irtkRealPixel>::max();
  min_z = voxel_limits<irtkRealPixel>::max();
  max_x = voxel_limits<irtkRealPixel>::min();
  max_y = voxel_limits<irtkRealPixel>::min();
  max_z = voxel_limits<irtkRealPixel>::min();
  double x, y, z;
  for (int i = 0; i <= stack.GetX(); i += stack.GetX())
    for (int j = 0; j <= stack.GetY(); j += stack.GetY())
      for (int k = 0; k <= stack.GetZ(); k += stack.GetZ()) {
    x = i;
    y = j;
    z = k;
    stack.ImageToWorld(x, y, z);
    // FIXME!!!
    transformation.Transform(x, y, z);
    //transformation.Inverse( x, y, z );
    if (x < min_x)
      min_x = x;
    if (y < min_y)
      min_y = y;
    if (z < min_z)
      min_z = z;
    if (x > max_x)
      max_x = x;
    if (y > max_y)
      max_y = y;
    if (z > max_z)
      max_z = z;
      }
}

void bboxCrop(irtkRealImage &image) {
  int min_x, min_y, min_z, max_x, max_y, max_z;
  min_x = image.GetX() - 1;
  min_y = image.GetY() - 1;
  min_z = image.GetZ() - 1;
  max_x = 0;
  max_y = 0;
  max_z = 0;
  for (int i = 0; i < image.GetX(); i++)
    for (int j = 0; j < image.GetY(); j++)
      for (int k = 0; k < image.GetZ(); k++) {
    if (image.Get(i, j, k) > 0) {
      if (i < min_x)
        min_x = i;
      if (j < min_y)
        min_y = j;
      if (k < min_z)
        min_z = k;
      if (i > max_x)
        max_x = i;
      if (j > max_y)
        max_y = j;
      if (k > max_z)
        max_z = k;
    }
      }

  //Cut region of interest
  image = image.GetRegion(min_x, min_y, min_z,
    max_x + 1, max_y + 1, max_z + 1);
}

void centroid(irtkRealImage &image,
  double &x,
  double &y,
  double &z) {
  double sum_x = 0;
  double sum_y = 0;
  double sum_z = 0;
  double norm = 0;
  double v;
  for (int i = 0; i < image.GetX(); i++)
    for (int j = 0; j < image.GetY(); j++)
      for (int k = 0; k < image.GetZ(); k++) {
    v = image.Get(i, j, k);
    if (v <= 0)
      continue;
    sum_x += v*i;
    sum_y += v*j;
    sum_z += v*k;
    norm += v;
      }

  x = sum_x / norm;
  y = sum_y / norm;
  z = sum_z / norm;

  image.ImageToWorld(x, y, z);

  std::cout << "CENTROID:" << x << "," << y << "," << z << "\n\n";
}

/*   end of auxiliary functions */

irtkReconstruction::irtkReconstruction(std::vector<int> dev, bool useCPUReg)
{
  _step = 0.0001;
  _debug = false;
  _quality_factor = 2;
  _sigma_bias = 12;
  _sigma_s_cpu = 0.025;
  _sigma_s_gpu = 0.025;
  _sigma_s2_cpu = 0.025;
  _sigma_s2_gpu = 0.025;
  _mix_s_cpu = 0.9;
  _mix_s_gpu = 0.9;
  _mix_cpu = 0.9;
  _mix_gpu = 0.9;
  _delta = 1;
  _lambda = 0.1;
  _alpha = (0.05 / _lambda) * _delta * _delta;
  _template_created = false;
  _have_mask = false;
  _low_intensity_cutoff = 0.01;
  _global_bias_correction = false;
  _adaptive = false;
  _use_SINC = false;

  int directions[13][3] = {
      { 1, 0, -1 },
      { 0, 1, -1 },
      { 1, 1, -1 },
      { 1, -1, -1 },
      { 1, 0, 0 },
      { 0, 1, 0 },
      { 1, 1, 0 },
      { 1, -1, 0 },
      { 1, 0, 1 },
      { 0, 1, 1 },
      { 1, 1, 1 },
      { 1, -1, 1 },
      { 0, 0, 1 }
  };
  for (int i = 0; i < 13; i++)
    for (int j = 0; j < 3; j++)
      _directions[i][j] = directions[i][j];

  _useCPUReg = useCPUReg;
  //FIXXME
  //TODO as possible workaround, but ugly -- dactiveates multithreading in cu
 // reconstructionGPU = new Reconstruction(dev, !_useCPUReg);
  /*if (_useCPUReg)
  {
    reconstructionGPU = new Reconstruction(dev, false); //workaround
  }
  else
  {*/
    reconstructionGPU = new Reconstruction(dev, true); //to produce the error for CPUReg and multithreaded GPUs
  //}

}


irtkReconstruction::~irtkReconstruction(){ }


void irtkReconstruction::Set_debugGPU(bool val)
{
  _debugGPU = val;
  reconstructionGPU->_debugGPU = val;
}



//////////////////////////////////////////////////////////////////////////////////
//GPU helpers
void irtkReconstruction::updateStackSizes(std::vector<uint3> stack_sizes_)
{
  reconstructionGPU->updateStackSizes(stack_sizes_);
}

void irtkReconstruction::SyncGPU()
{
  irtkGenericImage<float> mask_float_ = _mask; //check this
  std::cout << "SyncGPU()" << std::endl;
  reconstructionGPU->InitReconstructionVolume(make_uint3(_reconstructed.GetX(), _reconstructed.GetY(), _reconstructed.GetZ()),
    make_float3(_reconstructed.GetXSize(), _reconstructed.GetYSize(), _reconstructed.GetZSize()), _reconstructed_gpu.GetPointerToVoxels(), _sigma_bias);
  reconstructionGPU->setMask(make_uint3(_mask.GetX(), _mask.GetY(), _mask.GetZ()), make_float3(_mask.GetXSize(), _mask.GetYSize(), _mask.GetZSize()),
    mask_float_.GetPointerToVoxels(), _sigma_bias);
  if (_debugGPU)
  {
    irtkGenericImage<float> smask;

    smask.Initialize(_mask.GetImageAttributes());
    reconstructionGPU->debugSmoothMask(smask.GetPointerToVoxels());
    char buffer[256];
    sprintf(buffer, "smaskGPU%i.nii", 0);
    smask.Write(buffer);
    _mask.Write("maskCPUGPU.nii");
  }

  uint3 reqVDims = make_uint3(INT_MIN, INT_MIN, 0);
  uint3 wasteVDims = make_uint3(INT_MAX, INT_MAX, 0);

  for (int i = 0; i < _slices.size(); i++)
  {
    reqVDims = make_uint3(max((int)reqVDims.x, _slices[i].GetX()), max((int)reqVDims.y, _slices[i].GetY()), _slices.size());
    wasteVDims = make_uint3(min((int)wasteVDims.x, _slices[i].GetX()), min((int)wasteVDims.y, _slices[i].GetY()), _slices.size());
  }

  double waste = (((reqVDims.x - wasteVDims.x) * (reqVDims.y - wasteVDims.y) * _slices.size())*sizeof(double)*5.0) / 1024.0;
  printf("GPU memory waste approx: %f KB with %d %d %d %d\n", waste, reqVDims.x, reqVDims.y, wasteVDims.x, wasteVDims.y);


  irtkGenericImage<float> combinedStacks(reqVDims.x, reqVDims.y, reqVDims.z);
  //

  //combinedStacks.Clear();

  //all the same dimensions
  reconstructionGPU->initStorageVolumes(reqVDims, make_float3(_slices[0].GetXSize(), _slices[0].GetYSize(), _slices[0].GetZSize()));
  vector<int> sizesX;
  vector<int> sizesY;
  std::vector<float3> slice_dims;
  combinedStacks = -1.0;


  float* ptr = combinedStacks.GetPointerToVoxels();
  for (int i = 0; i < _slices.size(); i++)
  {
    irtkRealImage slice = _slices[i];
    //We need to do this line wise because of different cropping sizes
    for (int y = 0; y < slice.GetY(); y++)
    {
      for (int x = 0; x < slice.GetX(); x++)
      {
        combinedStacks(x, y, i) = slice(x, y, 0);
      }
      ptr += combinedStacks.GetX();
    }
    ptr += abs(combinedStacks.GetY() - slice.GetY())*combinedStacks.GetX();
    //TODO - set slice sizes
    sizesX.push_back(slice.GetX());
    sizesY.push_back(slice.GetY());
    slice_dims.push_back(make_float3(_slices[i].GetXSize(), _slices[i].GetYSize(), _slices[i].GetZSize()));
  }
  //combinedStacks.Write("combinedStacks.nii");

  reconstructionGPU->FillSlices(combinedStacks.GetPointerToVoxels(), sizesX, sizesY);

  reconstructionGPU->setSliceDims(slice_dims, _quality_factor);
  reconstructionGPU->reconstructedVoxelSize = _reconstructed.GetXSize();
  //	reconstructionGPU->UpdateScaleVector(_scale_gpu, _slice_weight_gpu);

  if(_debugGPU)
  {
    irtkGenericImage<float> debugSlices(reqVDims.x, reqVDims.y, reqVDims.z);
    reconstructionGPU->getSlicesVol_debug(debugSlices.GetPointerToVoxels());
    debugSlices.Write("debugSlices.nii");
    cudaDeviceSynchronize();
}
}

Matrix4 irtkReconstruction::toMatrix4(irtkMatrix mat)
{
  Matrix4 mmat;
  /*mmat.data[0] = make_double4(mat(0,0), mat(0,1), mat(0,2), mat(0,3));
  mmat.data[1] = make_double4(mat(1,0), mat(1,1), mat(1,2), mat(1,3));
  mmat.data[2] = make_double4(mat(2,0), mat(2,1), mat(2,2), mat(2,3));
  mmat.data[3] = make_double4(mat(3,0), mat(3,1), mat(3,2), mat(3,3));*/
  mmat.data[0] = make_float4(mat(0, 0), mat(0, 1), mat(0, 2), mat(0, 3));
  mmat.data[1] = make_float4(mat(1, 0), mat(1, 1), mat(1, 2), mat(1, 3));
  mmat.data[2] = make_float4(mat(2, 0), mat(2, 1), mat(2, 2), mat(2, 3));
  mmat.data[3] = make_float4(mat(3, 0), mat(3, 1), mat(3, 2), mat(3, 3));
  return mmat;
}

irtkMatrix irtkReconstruction::fromMatrix4(Matrix4 mat)
{
  irtkMatrix mmat;
  mmat.Initialize(4, 4);
  mmat.Ident();
  mmat.Put(0, 0, mat.data[0].x);
  mmat.Put(0, 1, mat.data[0].y);
  mmat.Put(0, 2, mat.data[0].z);
  mmat.Put(0, 3, mat.data[0].w);

  mmat.Put(1, 0, mat.data[1].x);
  mmat.Put(1, 1, mat.data[1].y);
  mmat.Put(1, 2, mat.data[1].z);
  mmat.Put(1, 3, mat.data[1].w);

  mmat.Put(2, 0, mat.data[2].x);
  mmat.Put(2, 1, mat.data[2].y);
  mmat.Put(2, 2, mat.data[2].z);
  mmat.Put(2, 3, mat.data[2].w);

  mmat.Put(3, 0, mat.data[3].x);
  mmat.Put(3, 1, mat.data[3].y);
  mmat.Put(3, 2, mat.data[3].z);
  mmat.Put(3, 3, mat.data[3].w);

  return mmat;
}

std::vector<irtkMatrix> irtkReconstruction::UpdateGPUTranformationMatrices()
{

  std::vector<Matrix4> sI2W;
  std::vector<Matrix4> sW2I;
  std::vector<Matrix4> sI2Winit;
  std::vector<Matrix4> sW2Iinit;
  std::vector<irtkMatrix> transformations_;
  std::vector<Matrix4> sliceTransforms_;
  std::vector<Matrix4> invsliceTransforms_;
  for (int i = 0; i < _slices.size(); i++)
  {
    //_transformations[i].UpdateMatrix();
    sI2Winit.push_back(toMatrix4(_slices[i].GetImageToWorldMatrix()));
    sW2Iinit.push_back(toMatrix4(_slices[i].GetWorldToImageMatrix()));
    sW2I.push_back(toMatrix4(_slices[i].GetWorldToImageMatrix()));
    sI2W.push_back(toMatrix4(_slices[i].GetImageToWorldMatrix()));
    sliceTransforms_.push_back(toMatrix4(_transformations_gpu[i].GetMatrix()));
    irtkMatrix invTrans = _transformations_gpu[i].GetMatrix();
    invTrans.Invert();
    invsliceTransforms_.push_back(toMatrix4(invTrans));

    transformations_.push_back(_transformations_gpu[i].GetMatrix());
  }

  reconstructionGPU->SetSliceMatrices(sliceTransforms_, invsliceTransforms_, sI2W, sW2I, sI2Winit, sW2Iinit,
    toMatrix4(_reconstructed.GetImageToWorldMatrix()), toMatrix4(_reconstructed.GetWorldToImageMatrix()));

  return transformations_;
}

//GPU helpers end
//////////////////////////////////////////////////////////////////////////////////



void irtkReconstruction::CenterStacks(vector<irtkRealImage>& stacks,
  vector<irtkRigidTransformation>& stack_transformations,
  int templateNumber) {
  // template center
  double x0, y0, z0;
  irtkRealImage mask;
  mask = stacks[templateNumber] != -1;
  centroid(mask, x0, y0, z0);

  double x, y, z;
  irtkMatrix m1, m2;
  for (unsigned int i = 0; i < stacks.size(); i++) {
    if (i == templateNumber)
      continue;

    mask = stacks[i] != -1;
    centroid(mask, x, y, z);

    irtkRigidTransformation translation;
    translation.PutTranslationX(x0 - x);
    translation.PutTranslationY(y0 - y);
    translation.PutTranslationZ(z0 - z);

    std::cout << "TRANSLATION:\n";
    translation.Print();
    std::cout << "\n\n\n";


    m1 = stack_transformations[i].GetMatrix();
    m2 = translation.GetMatrix();
    stack_transformations[i].PutMatrix(m2*m1);
  }

}

class ParallelAverage{
  irtkReconstruction* reconstructor;
  vector<irtkRealImage> &stacks;
  vector<irtkRigidTransformation> &stack_transformations;

  /// Padding value in target (voxels in the target image with this
  /// value will be ignored)
  double targetPadding;

  /// Padding value in source (voxels outside the source image will
  /// be set to this value)
  double sourcePadding;

  double background;

  // Volumetric registrations are stack-to-template while slice-to-volume 
  // registrations are actually performed as volume-to-slice
  // (reasons: technicalities of implementation)
  // so transformations need to be inverted beforehand.

  bool linear;

public:
  irtkRealImage average;
  irtkRealImage weights;

  void operator()(const blocked_range<size_t>& r) {
    for (size_t i = r.begin(); i < r.end(); ++i) {
      irtkImageTransformation imagetransformation;
      irtkImageFunction *interpolator;
      if (linear)
        interpolator = new irtkLinearInterpolateImageFunction;
      else
        interpolator = new irtkNearestNeighborInterpolateImageFunction;

      irtkRealImage s = stacks[i];
      irtkRigidTransformation t = stack_transformations[i];
      imagetransformation.SetInput(&s, &t);
      irtkRealImage image(reconstructor->_reconstructed.GetImageAttributes());
      image = 0;

      imagetransformation.SetOutput(&image);
      imagetransformation.PutTargetPaddingValue(targetPadding);
      imagetransformation.PutSourcePaddingValue(sourcePadding);
      imagetransformation.PutInterpolator(interpolator);
      imagetransformation.Run();

      irtkRealPixel *pa = average.GetPointerToVoxels();
      irtkRealPixel *pi = image.GetPointerToVoxels();
      irtkRealPixel *pw = weights.GetPointerToVoxels();
      for (int p = 0; p < average.GetNumberOfVoxels(); p++) {
        if (*pi != background) {
          *pa += *pi;
          *pw += 1;
        }
        pa++;
        pi++;
        pw++;
      }
      delete interpolator;
    }
  }

  ParallelAverage(ParallelAverage& x, split) :
    reconstructor(x.reconstructor),
    stacks(x.stacks),
    stack_transformations(x.stack_transformations)
  {
    average.Initialize(reconstructor->_reconstructed.GetImageAttributes());
    average = 0;
    weights.Initialize(reconstructor->_reconstructed.GetImageAttributes());
    weights = 0;
    targetPadding = x.targetPadding;
    sourcePadding = x.sourcePadding;
    background = x.background;
    linear = x.linear;
  }

  void join(const ParallelAverage& y) {
    average += y.average;
    weights += y.weights;
  }

  ParallelAverage(irtkReconstruction *reconstructor,
    vector<irtkRealImage>& _stacks,
    vector<irtkRigidTransformation>& _stack_transformations,
    double _targetPadding,
    double _sourcePadding,
    double _background,
    bool _linear = false) :
    reconstructor(reconstructor),
    stacks(_stacks),
    stack_transformations(_stack_transformations)
  {
    average.Initialize(reconstructor->_reconstructed.GetImageAttributes());
    average = 0;
    weights.Initialize(reconstructor->_reconstructed.GetImageAttributes());
    weights = 0;
    targetPadding = _targetPadding;
    sourcePadding = _sourcePadding;
    background = _background;
    linear = _linear;
  }

  // execute
  void operator() () {
    task_scheduler_init init(tbb_no_threads);
    parallel_reduce(blocked_range<size_t>(0, stacks.size()),
      *this);
    init.terminate();
  }
};

class ParallelSliceAverage{
  irtkReconstruction* reconstructor;
  vector<irtkRealImage> &slices;
  vector<irtkRigidTransformation> &slice_transformations;
  irtkRealImage &average;
  irtkRealImage &weights;

public:

  void operator()(const blocked_range<size_t>& r) const {
    for (int k0 = r.begin(); k0 < r.end(); k0++) {
      for (int j0 = 0; j0 < average.GetY(); j0++) {
        for (int i0 = 0; i0 < average.GetX(); i0++) {
          double x0 = i0;
          double y0 = j0;
          double z0 = k0;
          // Transform point into world coordinates
          average.ImageToWorld(x0, y0, z0);
          for (int inputIndex = 0; inputIndex < slices.size(); inputIndex++) {
            double x = x0;
            double y = y0;
            double z = z0;
            // Transform point
            slice_transformations[inputIndex].Transform(x, y, z);
            // Transform point into image coordinates
            slices[inputIndex].WorldToImage(x, y, z);
            int i = round(x);
            int j = round(y);
            int k = round(z);
            // Check whether transformed point is in FOV of input
            if ((i >= 0) && (i < slices[inputIndex].GetX()) &&
              (j >= 0) && (j < slices[inputIndex].GetY()) &&
              (k >= 0) && (k < slices[inputIndex].GetZ())) {
              if (slices[inputIndex](i, j, k) > 0) {
                average.PutAsDouble(i0, j0, k0, 0, average(i0, j0, k0) + slices[inputIndex](i, j, k));
                weights.PutAsDouble(i0, j0, k0, 0, weights(i0, j0, k0) + 1);
              }
            }
          }
        }
      }
    }
  }

  ParallelSliceAverage(irtkReconstruction *reconstructor,
    vector<irtkRealImage>& _slices,
    vector<irtkRigidTransformation>& _slice_transformations,
    irtkRealImage &_average,
    irtkRealImage &_weights) :
    reconstructor(reconstructor),
    slices(_slices),
    slice_transformations(_slice_transformations),
    average(_average),
    weights(_weights)
  {
    average.Initialize(reconstructor->_reconstructed.GetImageAttributes());
    average = 0;
    weights.Initialize(reconstructor->_reconstructed.GetImageAttributes());
    weights = 0;
  }

  // execute
  void operator() () const {
    task_scheduler_init init(tbb_no_threads);
    parallel_for(blocked_range<size_t>(0, average.GetZ()),
      *this);
    init.terminate();
  }
};

irtkRealImage irtkReconstruction::CreateAverage(vector<irtkRealImage>& stacks,
  vector<irtkRigidTransformation>& stack_transformations)
{
  if (!_template_created) {
    cerr << "Please create the template before calculating the average of the stacks." << endl;
    exit(1);
  }

  InvertStackTransformations(stack_transformations);
  ParallelAverage parallelAverage(this,
    stacks,
    stack_transformations,
    -1, 0, 0, // target/source/background
    true);
  parallelAverage();
  irtkRealImage average = parallelAverage.average;
  irtkRealImage weights = parallelAverage.weights;
  average /= weights;
  InvertStackTransformations(stack_transformations);
  return average;
}

double irtkReconstruction::CreateTemplate(irtkRealImage stack, double resolution)
{
  double dx, dy, dz, d;

  //Get image attributes - image size and voxel size
  irtkImageAttributes attr = stack.GetImageAttributes();

  //enlarge stack in z-direction in case top of the head is cut off
  attr._z += 2;

  //create enlarged image
  irtkRealImage enlarged(attr);

  //determine resolution of volume to reconstruct
  if (resolution <= 0) {
    //resolution was not given by user
    // set it to min of res in x or y direction
    stack.GetPixelSize(&dx, &dy, &dz);
    if ((dx <= dy) && (dx <= dz))
      d = dx;
    else if (dy <= dz)
      d = dy;
    else
      d = dz;
  }
  else
    d = resolution;

  cout << "Constructing volume with isotropic voxel size " << d << endl;

  //resample "enlarged" to resolution "d"
  irtkNearestNeighborInterpolateImageFunction interpolator;
  irtkResampling<irtkRealPixel> resampling(d, d, d);
  resampling.SetInput(&enlarged);
  resampling.SetOutput(&enlarged);
  resampling.SetInterpolator(&interpolator);
  resampling.Run();

  //initialize recontructed volume
  _reconstructed = enlarged;

  _reconstructed_gpu.Initialize(_reconstructed.GetImageAttributes());
  _template_created = true;

  //return resulting resolution of the template image
  return d;
}

irtkRealImage irtkReconstruction::CreateMask(irtkRealImage image)
{
  //binarize mask
  irtkRealPixel* ptr = image.GetPointerToVoxels();
  for (int i = 0; i < image.GetNumberOfVoxels(); i++) {
    if (*ptr > 0.0)
      *ptr = 1;
    else
      *ptr = 0;
    ptr++;
  }
  return image;
}

void irtkReconstruction::SetMask(irtkRealImage * mask, double sigma, double threshold)
{
  if (!_template_created) {
    cerr
      << "Please create the template before setting the mask, so that the mask can be resampled to the correct dimensions."
      << endl;
    exit(1);
  }

  _mask = _reconstructed;

  if (mask != NULL) {
    //if sigma is nonzero first smooth the mask
    if (sigma > 0) {
      //blur mask
      irtkGaussianBlurring<irtkRealPixel> gb(sigma);
      gb.SetInput(mask);
      gb.SetOutput(mask);
      gb.Run();

      //binarize mask
      irtkRealPixel* ptr = mask->GetPointerToVoxels();
      for (int i = 0; i < mask->GetNumberOfVoxels(); i++) {
        if (*ptr > threshold)
          *ptr = 1;
        else
          *ptr = 0;
        ptr++;
      }
    }

    //resample the mask according to the template volume using identity transformation
    irtkRigidTransformation transformation;
    irtkImageTransformation imagetransformation;
    irtkNearestNeighborInterpolateImageFunction interpolator;
    imagetransformation.SetInput(mask, &transformation);
    imagetransformation.SetOutput(&_mask);
    //target is zero image, need padding -1
    imagetransformation.PutTargetPaddingValue(-1);
    //need to fill voxels in target where there is no info from source with zeroes
    imagetransformation.PutSourcePaddingValue(0);
    imagetransformation.PutInterpolator(&interpolator);
    imagetransformation.Run();
  }
  else {
    //fill the mask with ones
    _mask = 1;
  }
  //set flag that mask was created
  _have_mask = true;

  if (_debug)
    _mask.Write("mask.nii");
}

void irtkReconstruction::TransformMask(irtkRealImage& image, irtkRealImage& mask,
  irtkRigidTransformation& transformation)
{
  //transform mask to the space of image
  irtkImageTransformation imagetransformation;
  irtkNearestNeighborInterpolateImageFunction interpolator;
  imagetransformation.SetInput(&mask, &transformation);
  irtkRealImage m = image;
  imagetransformation.SetOutput(&m);
  //target contains zeros and ones image, need padding -1
  imagetransformation.PutTargetPaddingValue(-1);
  //need to fill voxels in target where there is no info from source with zeroes
  imagetransformation.PutSourcePaddingValue(0);
  imagetransformation.PutInterpolator(&interpolator);
  imagetransformation.Run();
  mask = m;
}

void irtkReconstruction::ResetOrigin(irtkGreyImage &image, irtkRigidTransformation& transformation)
{
  double ox, oy, oz;
  image.GetOrigin(ox, oy, oz);
  image.PutOrigin(0, 0, 0);
  transformation.PutTranslationX(ox);
  transformation.PutTranslationY(oy);
  transformation.PutTranslationZ(oz);
  transformation.PutRotationX(0);
  transformation.PutRotationY(0);
  transformation.PutRotationZ(0);
}

void irtkReconstruction::ResetOrigin(irtkRealImage &image, irtkRigidTransformation& transformation)
{
  double ox, oy, oz;
  image.GetOrigin(ox, oy, oz);
  image.PutOrigin(0, 0, 0);
  transformation.PutTranslationX(ox);
  transformation.PutTranslationY(oy);
  transformation.PutTranslationZ(oz);
  transformation.PutRotationX(0);
  transformation.PutRotationY(0);
  transformation.PutRotationZ(0);
}

class ParallelStackRegistrations {
  irtkReconstruction *reconstructor;
  vector<irtkRealImage>& stacks;
  vector<irtkRigidTransformation>& stack_transformations;
  int templateNumber;
  irtkGreyImage& target;
  irtkRigidTransformation& offset;
  bool _externalTemplate;

public:
  ParallelStackRegistrations(irtkReconstruction *_reconstructor,
    vector<irtkRealImage>& _stacks,
    vector<irtkRigidTransformation>& _stack_transformations,
    int _templateNumber,
    irtkGreyImage& _target,
    irtkRigidTransformation& _offset,
    bool externalTemplate = false) :
    reconstructor(_reconstructor),
    stacks(_stacks),
    stack_transformations(_stack_transformations),
    target(_target),
    offset(_offset) {
    templateNumber = _templateNumber,
      _externalTemplate = externalTemplate;
  }

  void operator() (const blocked_range<size_t> &r) const {
    for (size_t i = r.begin(); i != r.end(); ++i) {

      //do not perform registration for template
      if (i == templateNumber)
        continue;

      //rigid registration object
      irtkImageRigidRegistrationWithPadding registration;
      //irtkRigidTransformation transformation = stack_transformations[i];

      //set target and source (need to be converted to irtkGreyImage)
      irtkGreyImage source = stacks[i];

      //include offset in trasformation   
      irtkMatrix mo = offset.GetMatrix();
      irtkMatrix m = stack_transformations[i].GetMatrix();
      m = m*mo;
      stack_transformations[i].PutMatrix(m);

      //perform rigid registration
      registration.SetInput(&target, &source);
      registration.SetOutput(&stack_transformations[i]);
      if (_externalTemplate)
      {
        registration.GuessParameterThickSlicesNMI();
      }
      else
      {
        registration.GuessParameterThickSlices();
      }
      registration.SetTargetPadding(0);
      registration.Run();

      mo.Invert();
      m = stack_transformations[i].GetMatrix();
      m = m*mo;
      stack_transformations[i].PutMatrix(m);

      //stack_transformations[i] = transformation;            

      //save volumetric registrations
      if (reconstructor->_debug) {
        //buffer to create the name
        char buffer[256];
        registration.irtkImageRegistration::Write((char *) "parout-volume.rreg");
        sprintf(buffer, "stack-transformation%i.dof.gz", i);
        stack_transformations[i].irtkTransformation::Write(buffer);
        target.Write("target.nii.gz");
        sprintf(buffer, "stack%i.nii.gz", i);
        stacks[i].Write(buffer);
      }
    }
  }

  // execute
  void operator() () const {
    task_scheduler_init init(tbb_no_threads);
    parallel_for(blocked_range<size_t>(0, stacks.size()),
      *this);
    init.terminate();
  }

};

void irtkReconstruction::StackRegistrations(vector<irtkRealImage>& stacks,
  vector<irtkRigidTransformation>& stack_transformations, int templateNumber, bool useExternalTarget)
{
  if (_debug)
    cout << "StackRegistrations" << endl;

  InvertStackTransformations(stack_transformations);

  //template is set as the target
  irtkGreyImage target;
  if (!useExternalTarget)
  {
    target = stacks[templateNumber];
  }
  else
  {
    target = externalRegistrationTargetImage;
  }

  //target needs to be masked before registration
  if (_have_mask) {
    double x, y, z;
    for (int i = 0; i < target.GetX(); i++)
      for (int j = 0; j < target.GetY(); j++)
        for (int k = 0; k < target.GetZ(); k++) {
      //image coordinates of the target
      x = i;
      y = j;
      z = k;
      //change to world coordinates
      target.ImageToWorld(x, y, z);
      //change to mask image coordinates - mask is aligned with target
      _mask.WorldToImage(x, y, z);
      x = round(x);
      y = round(y);
      z = round(z);
      //if the voxel is outside mask ROI set it to -1 (padding value)
      if ((x >= 0) && (x < _mask.GetX()) && (y >= 0) && (y < _mask.GetY()) && (z >= 0)
        && (z < _mask.GetZ())) {
        if (_mask(x, y, z) == 0)
          target(i, j, k) = 0;
      }
      else
        target(i, j, k) = 0;
        }
  }

  // apply template transformation
  headertool_dofin( target, stack_transformations[templateNumber] );
  
  irtkRigidTransformation offset;
  ResetOrigin(target, offset);

  //register all stacks to the target
  ParallelStackRegistrations registration(this,
    stacks,
    stack_transformations,
    templateNumber,
    target,
    offset,
    useExternalTarget);
  registration();

  InvertStackTransformations(stack_transformations);
}

void irtkReconstruction::RestoreSliceIntensities()
{
  if (_debug)
    cout << "Restoring the intensities of the slices. " << endl;

  unsigned int inputIndex;
  int i;
  double factor;
  irtkRealPixel *p;

  for (inputIndex = 0; inputIndex < _slices.size(); inputIndex++) {
    //calculate scaling factor
    factor = _stack_factor[_stack_index[inputIndex]];//_average_value;

    // read the pointer to current slice
    p = _slices[inputIndex].GetPointerToVoxels();
    for (i = 0; i < _slices[inputIndex].GetNumberOfVoxels(); i++) {
      if (*p>0) *p = *p / factor;
      p++;
    }
  }
}

void irtkReconstruction::RestoreSliceIntensitiesGPU()
{
  //TODO
  //_stack_factor
  reconstructionGPU->RestoreSliceIntensities(_stack_factor, _stack_index);

}

void irtkReconstruction::ScaleVolume()
{
  if (_debug)
    cout << "Scaling volume: ";

  unsigned int inputIndex;
  int i, j;
  double scalenum = 0, scaleden = 0;

  for (inputIndex = 0; inputIndex < _slices.size(); inputIndex++) {
    // alias for the current slice
    irtkRealImage& slice = _slices[inputIndex];

    //alias for the current weight image
    irtkRealImage& w = _weights[inputIndex];

    // alias for the current simulated slice
    irtkRealImage& sim = _simulated_slices[inputIndex];

    for (i = 0; i < slice.GetX(); i++)
      for (j = 0; j < slice.GetY(); j++)
        if (slice(i, j, 0) != -1) {
      //scale - intensity matching
      if (_simulated_weights[inputIndex](i, j, 0) > 0.99) {
        scalenum += w(i, j, 0) * _slice_weight_cpu[inputIndex] * slice(i, j, 0) * sim(i, j, 0);
        scaleden += w(i, j, 0) * _slice_weight_cpu[inputIndex] * sim(i, j, 0) * sim(i, j, 0);
      }
        }
  } //end of loop for a slice inputIndex

  //calculate scale for the volume
  double scale = scalenum / scaleden;
  printf("Volume scale CPU: %f\n", scale);

  if (_debug)
    cout << " scale = " << scale;

  irtkRealPixel *ptr = _reconstructed.GetPointerToVoxels();
  for (i = 0; i < _reconstructed.GetNumberOfVoxels(); i++) {
    if (*ptr>0) *ptr = *ptr * scale;
    ptr++;
  }
  cout << endl;
}



void irtkReconstruction::ScaleVolumeGPU()
{
  if (_debug)
    cout << "Scaling volume: ";

  //TODO
  reconstructionGPU->ScaleVolume();
}

class ParallelSimulateSlices {
  irtkReconstruction *reconstructor;

public:
  ParallelSimulateSlices(irtkReconstruction *_reconstructor) :
    reconstructor(_reconstructor) { }

  void operator() (const blocked_range<size_t> &r) const {
    for (size_t inputIndex = r.begin(); inputIndex != r.end(); ++inputIndex) {
      //Calculate simulated slice
      reconstructor->_simulated_slices[inputIndex].Initialize(reconstructor->_slices[inputIndex].GetImageAttributes());
      reconstructor->_simulated_slices[inputIndex] = 0;

      reconstructor->_simulated_weights[inputIndex].Initialize(reconstructor->_slices[inputIndex].GetImageAttributes());
      reconstructor->_simulated_weights[inputIndex] = 0;

      reconstructor->_simulated_inside[inputIndex].Initialize(reconstructor->_slices[inputIndex].GetImageAttributes());
      reconstructor->_simulated_inside[inputIndex] = 0;

      reconstructor->_slice_inside_cpu[inputIndex] = false;

      POINT3D p;
      for (unsigned int i = 0; i < reconstructor->_slices[inputIndex].GetX(); i++)
        for (unsigned int j = 0; j < reconstructor->_slices[inputIndex].GetY(); j++)
          if (reconstructor->_slices[inputIndex](i, j, 0) != -1) {
        double weight = 0;
        int n = reconstructor->_volcoeffs[inputIndex][i][j].size();
        for (unsigned int k = 0; k < n; k++) {
          p = reconstructor->_volcoeffs[inputIndex][i][j][k];
          reconstructor->_simulated_slices[inputIndex](i, j, 0) += p.value * reconstructor->_reconstructed(p.x, p.y, p.z);
          weight += p.value;
          if (reconstructor->_mask(p.x, p.y, p.z) == 1) {
            reconstructor->_simulated_inside[inputIndex](i, j, 0) = 1;
            reconstructor->_slice_inside_cpu[inputIndex] = true;
          }
        }
        if (weight > 0) {
          reconstructor->_simulated_slices[inputIndex](i, j, 0) /= weight;
          reconstructor->_simulated_weights[inputIndex](i, j, 0) = weight;
        }
          }

    }
  }

  // execute
  void operator() () const {
    task_scheduler_init init(tbb_no_threads);
    parallel_for(blocked_range<size_t>(0, reconstructor->_slices.size()),
      *this);
    init.terminate();
  }

};


void irtkReconstruction::SimulateSlices()
{
  if (_debug)
    cout << "Simulating slices." << endl;

  ParallelSimulateSlices parallelSimulateSlices(this);
  parallelSimulateSlices();

  if (_debug)
    cout << "done." << endl;
  if(_debugGPU)
  {
    _simulated_weights[40].Write("testsimweights40.nii");
    _simulated_slices[40].Write("testsimslices40.nii");
}
}

void irtkReconstruction::SimulateSlicesGPU()
{
  if (_slice_inside_gpu.size() == 0)
  {
    _slice_inside_gpu.clear();
    _slice_inside_gpu.resize(_slices.size());
  }

  //debug sync
  //reconstructionGPU->syncGPUrecon(_reconstructed.GetPointerToVoxels());

  reconstructionGPU->SimulateSlices(_slice_inside_gpu);

  //debug sync
  //SyncSimSlicesGPU2CPU();
  if(_debugGPU)
  {
    cudaDeviceSynchronize();

    irtkGenericImage<float> simslicesV(reconstructionGPU->v_simulated_slices.size.x, reconstructionGPU->v_simulated_slices.size.y, reconstructionGPU->v_simulated_slices.size.z);
    irtkGenericImage<float> simweightsV(reconstructionGPU->v_simulated_slices.size.x, reconstructionGPU->v_simulated_slices.size.y, reconstructionGPU->v_simulated_slices.size.z);

    reconstructionGPU->debugSimslices(simslicesV.GetPointerToVoxels());
    simslicesV.Write("simslicesV.nii");


    reconstructionGPU->debugSimweights(simweightsV.GetPointerToVoxels());
    simweightsV.Write("simweightsV.nii");

    irtkGenericImage<char> siminsideV(reconstructionGPU->v_simulated_slices.size.x, reconstructionGPU->v_simulated_slices.size.y, reconstructionGPU->v_simulated_slices.size.z);
    reconstructionGPU->debugSiminside(siminsideV.GetPointerToVoxels());
    siminsideV.Write("siminsideV.nii");

    std::cout << "_slice_inside_gpu: ";
    for (int i = 0; i < _slice_inside_gpu.size(); i++)
    {
      std::cout << _slice_inside_gpu[i] << " ";
    }
    std::cout << std::endl;
}
}

void irtkReconstruction::SimulateStacks(vector<irtkRealImage>& stacks)
{
  if (_debug)
    cout << "Simulating stacks." << endl;

  unsigned int inputIndex;
  int i, j, k, n;
  irtkRealImage sim;
  POINT3D p;
  double weight;

  int z, current_stack;
  z = -1;//this is the z coordinate of the stack
  current_stack = -1; //we need to know when to start a new stack


  for (inputIndex = 0; inputIndex < _slices.size(); inputIndex++) {


    // read the current slice
    irtkRealImage& slice = _slices[inputIndex];

    //Calculate simulated slice
    sim.Initialize(slice.GetImageAttributes());
    sim = 0;

    //do not simulate excluded slice
    if (_slice_weight_cpu[inputIndex]>0.5)
    {
      for (i = 0; i < slice.GetX(); i++)
        for (j = 0; j < slice.GetY(); j++)
          if (slice(i, j, 0) != -1) {
        weight = 0;
        n = _volcoeffs[inputIndex][i][j].size();
        for (k = 0; k < n; k++) {
          p = _volcoeffs[inputIndex][i][j][k];
          sim(i, j, 0) += p.value * _reconstructed(p.x, p.y, p.z);
          weight += p.value;
        }
        if (weight>0)
          sim(i, j, 0) /= weight;
          }
    }

    if (_stack_index[inputIndex] == current_stack)
      z++;
    else {
      current_stack = _stack_index[inputIndex];
      z = 0;
    }

    for (i = 0; i < sim.GetX(); i++)
      for (j = 0; j < sim.GetY(); j++) {
      stacks[_stack_index[inputIndex]](i, j, z) = sim(i, j, 0);
      }
    //end of loop for a slice inputIndex
  }
}

void irtkReconstruction::MatchStackIntensities(vector<irtkRealImage>& stacks,
  vector<irtkRigidTransformation>& stack_transformations, double averageValue, bool together)
{
  if (_debug)
    cout << "Matching intensities of stacks. ";

  //Calculate the averages of intensities for all stacks
  double sum, num;
  char buffer[256];
  unsigned int ind;
  int i, j, k;
  double x, y, z;
  vector<double> stack_average;

  //remember the set average value
  _average_value = averageValue;

  //averages need to be calculated only in ROI
  for (ind = 0; ind < stacks.size(); ind++) {
    sum = 0;
    num = 0;
    for (i = 0; i < stacks[ind].GetX(); i++)
      for (j = 0; j < stacks[ind].GetY(); j++)
        for (k = 0; k < stacks[ind].GetZ(); k++) {
      //image coordinates of the stack voxel
      x = i;
      y = j;
      z = k;
      //change to world coordinates
      stacks[ind].ImageToWorld(x, y, z);
      //transform to template (and also _mask) space
      stack_transformations[ind].Transform(x, y, z);
      //change to mask image coordinates - mask is aligned with template
      _mask.WorldToImage(x, y, z);
      x = round(x);
      y = round(y);
      z = round(z);
      //if the voxel is inside mask ROI include it
      // if ((x >= 0) && (x < _mask.GetX()) && (y >= 0) && (y < _mask.GetY()) && (z >= 0)
      //      && (z < _mask.GetZ()))
      //      {
      //if (_mask(x, y, z) == 1)
      if (stacks[ind](i, j, k) > 0) {
        sum += stacks[ind](i, j, k);
        num++;
      }
      //}
        }
    //calculate average for the stack
    if (num > 0)
      stack_average.push_back(sum / num);
    else {
      cerr << "Stack " << ind << " has no overlap with ROI" << endl;
      exit(1);
    }
  }

  double global_average;
  if (together) {
    global_average = 0;
    for (i = 0; i < stack_average.size(); i++)
      global_average += stack_average[i];
    global_average /= stack_average.size();
  }

  if (_debug) {
    cout << "Stack average intensities are ";
    for (ind = 0; ind < stack_average.size(); ind++)
      cout << stack_average[ind] << " ";
    cout << endl;
    cout << "The new average value is " << averageValue << endl;
  }

  //Rescale stacks
  irtkRealPixel *ptr;
  double factor;
  for (ind = 0; ind < stacks.size(); ind++) {
    if (together) {
      factor = averageValue / global_average;
      _stack_factor.push_back(factor);
    }
    else {
      factor = averageValue / stack_average[ind];
      _stack_factor.push_back(factor);

    }

    ptr = stacks[ind].GetPointerToVoxels();
    for (i = 0; i < stacks[ind].GetNumberOfVoxels(); i++) {
      if (*ptr > 0)
        *ptr *= factor;
      ptr++;
    }
  }

  if (_debug) {
    for (ind = 0; ind < stacks.size(); ind++) {
      sprintf(buffer, "rescaled-stack%i.nii.gz", ind);
      stacks[ind].Write(buffer);
    }

    cout << "Slice intensity factors are ";
    for (ind = 0; ind < stack_average.size(); ind++)
      cout << _stack_factor[ind] << " ";
    cout << endl;
    cout << "The new average value is " << averageValue << endl;
  }

}


void irtkReconstruction::MatchStackIntensitiesWithMasking(vector<irtkRealImage>& stacks,
  vector<irtkRigidTransformation>& stack_transformations, double averageValue, bool together)
{
  if (_debug)
    cout << "Matching intensities of stacks. ";

  //Calculate the averages of intensities for all stacks
  double sum, num;
  char buffer[256];
  unsigned int ind;
  int i, j, k;
  double x, y, z;
  vector<double> stack_average;
  irtkRealImage m;

  //remember the set average value
  _average_value = averageValue;

  //averages need to be calculated only in ROI
  for (ind = 0; ind < stacks.size(); ind++) {
    m = stacks[ind];
    sum = 0;
    num = 0;
    for (i = 0; i < stacks[ind].GetX(); i++)
      for (j = 0; j < stacks[ind].GetY(); j++)
        for (k = 0; k < stacks[ind].GetZ(); k++) {
      //image coordinates of the stack voxel
      x = i;
      y = j;
      z = k;
      //change to world coordinates
      stacks[ind].ImageToWorld(x, y, z);
      //transform to template (and also _mask) space
      stack_transformations[ind].Transform(x, y, z);
      //change to mask image coordinates - mask is aligned with template
      _mask.WorldToImage(x, y, z);
      x = round(x);
      y = round(y);
      z = round(z);
      //if the voxel is inside mask ROI include it
      if ((x >= 0) && (x < _mask.GetX()) && (y >= 0) && (y < _mask.GetY()) && (z >= 0)
        && (z < _mask.GetZ()))
      {
        if (_mask(x, y, z) == 1)
        {
          m(i, j, k) = 1;
          sum += stacks[ind](i, j, k);
          num++;
        }
        else
          m(i, j, k) = 0;
      }
        }
    if (_debug)
    {
      sprintf(buffer, "mask-for-matching%i.nii.gz", ind);
      m.Write(buffer);
    }
    //calculate average for the stack
    if (num > 0)
      stack_average.push_back(sum / num);
    else {
      cerr << "Stack " << ind << " has no overlap with ROI" << endl;
      exit(1);
    }
  }

  double global_average;
  if (together) {
    global_average = 0;
    for (i = 0; i < stack_average.size(); i++)
      global_average += stack_average[i];
    global_average /= stack_average.size();
  }

  if (_debug) {
    cout << "Stack average intensities are ";
    for (ind = 0; ind < stack_average.size(); ind++)
      cout << stack_average[ind] << " ";
    cout << endl;
    cout << "The new average value is " << averageValue << endl;
  }

  //Rescale stacks
  irtkRealPixel *ptr;
  double factor;
  for (ind = 0; ind < stacks.size(); ind++) {
    if (together) {
      factor = averageValue / global_average;
      _stack_factor.push_back(factor);
    }
    else {
      factor = averageValue / stack_average[ind];
      _stack_factor.push_back(factor);

    }

    ptr = stacks[ind].GetPointerToVoxels();
    for (i = 0; i < stacks[ind].GetNumberOfVoxels(); i++) {
      if (*ptr > 0)
        *ptr *= factor;
      ptr++;
    }
  }

  if (_debug) {
    for (ind = 0; ind < stacks.size(); ind++) {
      sprintf(buffer, "rescaled-stack%i.nii.gz", ind);
      stacks[ind].Write(buffer);
    }

    cout << "Slice intensity factors are ";
    for (ind = 0; ind < stack_average.size(); ind++)
      cout << _stack_factor[ind] << " ";
    cout << endl;
    cout << "The new average value is " << averageValue << endl;
  }

}


void irtkReconstruction::generatePSFVolume()
{
  double dx, dy, dz;
  //currentlz just a test function
  _slices[_slices.size() - 1].GetPixelSize(&dx, &dy, &dz);

  //sigma of 3D Gaussian (sinc with FWHM=dx or dy in-plane, Gaussian with FWHM = dz through-plane)
  //double psfSigma = 2.3548*((double)PSF_SIZE/2.0);
  double psfSigma = 2.3548;

  double size = _reconstructed.GetXSize() / _quality_factor;

  //TODO TUne until same as CPU
  //number of voxels in each direction
  //the ROI is 2*voxel dimension

  int xDim = PSF_SIZE;//round(2 * dx / size);
  int yDim = PSF_SIZE;
  int zDim = PSF_SIZE;

  double ldim;
  ldim = xDim;
  ldim = (ldim > yDim) ? ldim : yDim;
  ldim = (ldim > zDim) ? ldim : zDim;
  double xsize = xDim / ldim;
  double ysize = yDim / ldim;
  double zsize = zDim / ldim;

  //psfSigma = psfSigma + psfSigma/xsize;

  //printf("%f %f %f\n",xsize, ysize, zsize);

  double sigmax = (1.2) * (dx) / (psfSigma);
  double sigmay = (1.2)  * (dy) / (psfSigma);
  double sigmaz = (1.0) * (dz) / (psfSigma);
  //printf("Sigmas orig: %f %f %f\n",sigmax, sigmay, sigmaz);

  //image corresponding to PSF
  irtkImageAttributes attr;
  attr._x = xDim;
  attr._y = yDim;
  attr._z = zDim;
  attr._dx = _reconstructed.GetXSize();
  attr._dy = _reconstructed.GetYSize();
  attr._dz = _reconstructed.GetZSize();
  //attr._dx = size;
  //attr._dy = size;
  //attr._dz = size;
  irtkGenericImage<float> PSF(attr);

  //centre of PSF
  double cx, cy, cz;
  cx = 0.5 * (attr._x - 1);
  cy = 0.5 * (attr._y - 1);
  cz = 0.5 * (attr._z - 1);
  PSF.ImageToWorld(cx, cy, cz);
  //PSF.GetImageToWorldMatrix().Print();
  //printf("%d %d %d \n\n", xDim, yDim, zDim);

  double x, y, z;
  double sum = 0;
  int i, j, k;
  for (i = 0; i < attr._x; i++)
    for (j = 0; j < attr._y; j++)
      for (k = 0; k < attr._z; k++) {
    x = i;
    y = j;
    z = k;

    //printf("%f %f %f \n", x, y, z);
    PSF.ImageToWorld(x, y, z);
    //printf("%f %f %f \n", x, y, z);

    x -= cx;
    y -= cy;
    z -= cz;

#if 0
    //Gauss
    PSF(i, j, k) = exp(-x * x / (2.0 * sigmax * sigmax) - y * y / (2.0 * sigmay * sigmay)
      - z * z / (2.0 * sigmaz * sigmaz));
#endif
#if 0
    //sinc
    double R = sqrt(x * x / (2 * sigmax * sigmax) + y * y / (2 * sigmay * sigmay)
      + z * z / (2 * sigmaz * sigmaz));
    PSF(i, j, k) = sin(R) / (R);
#endif
#if 1
    //sinc gauss
    double R = sqrt(x * x / (2 * sigmax * sigmax) + y * y / (2 * sigmay * sigmay)
      + z * z / (2 * sigmaz * sigmaz));
    PSF(i, j, k) = sin(R) / (R)* exp(-x * x / (2 * sigmax * sigmax) - y * y / (2 * sigmay * sigmay)
      - z * z / (2 * sigmaz * sigmaz));
#endif
    sum += PSF(i, j, k);
      }
  PSF /= sum;


  // PSF.Write("PSFtest.nii");
  if (_debugGPU)
  {
    PSF.Write("PSFtest.nii");
    PSF.GetWorldToImageMatrix().Print();
    PSF.GetImageToWorldMatrix().Print();
  }

  //PSF = PSFn;
  reconstructionGPU->generatePSFVolume(PSF.GetPointerToVoxels(),
    make_uint3(PSF.GetX(), PSF.GetY(), PSF.GetZ()),
    make_float3(_slices[0].GetXSize(), _slices[0].GetYSize(), _slices[0].GetZSize()),
    make_float3(PSF.GetXSize(), PSF.GetYSize(), PSF.GetZSize()), toMatrix4(PSF.GetImageToWorldMatrix()),
    toMatrix4(PSF.GetWorldToImageMatrix()), _quality_factor, _use_SINC);
}

void irtkReconstruction::CreateSlicesAndTransformations(vector<irtkRealImage> &stacks,
  vector<irtkRigidTransformation> &stack_transformations,
  vector<double> &thickness,
  const vector<irtkRealImage> &probability_maps)
{
  if (_debug)
    cout << "CreateSlicesAndTransformations" << endl;

  std::vector<uint3> stack_sizes_;
  //for each stack
  for (unsigned int i = 0; i < stacks.size(); i++) {
    //image attributes contain image and voxel size
    irtkImageAttributes attr = stacks[i].GetImageAttributes();
    //printf("stack sizes z: %d \n", attr._z);
    stack_sizes_.push_back(make_uint3(attr._x, attr._y, attr._z));
    //attr._z is number of slices in the stack
    for (int j = 0; j < attr._z; j++) {
      //create slice by selecting the appropreate region of the stack
      irtkRealImage slice = stacks[i].GetRegion(0, 0, j, attr._x, attr._y, j + 1);
      //set correct voxel size in the stack. Z size is equal to slice thickness.
      slice.PutPixelSize(attr._dx, attr._dy, thickness[i]);
      //remember the slice
      _slices.push_back(slice);
      _simulated_slices.push_back(slice);
      _simulated_weights.push_back(slice);
      _simulated_inside.push_back(slice);
      //remeber stack index for this slice
      _stack_index.push_back(i);
      //initialize slice transformation with the stack transformation
      _transformations.push_back(stack_transformations[i]);
      _transformations_gpu.push_back(stack_transformations[i]);
    }
  }
  reconstructionGPU->updateStackSizes(stack_sizes_);

  cout << "Number of slices: " << _slices.size() << endl;
}

void irtkReconstruction::ResetSlices(vector<irtkRealImage>& stacks,
  vector<double>& thickness)
{
  if (_debug)
    cout << "ResetSlices" << endl;

  _slices.clear();

  //for each stack
  for (unsigned int i = 0; i < stacks.size(); i++) {
    //image attributes contain image and voxel size
    irtkImageAttributes attr = stacks[i].GetImageAttributes();

    //attr._z is number of slices in the stack
    for (int j = 0; j < attr._z; j++) {
      //create slice by selecting the appropreate region of the stack
      irtkRealImage slice = stacks[i].GetRegion(0, 0, j, attr._x, attr._y, j + 1);
      //set correct voxel size in the stack. Z size is equal to slice thickness.
      slice.PutPixelSize(attr._dx, attr._dy, thickness[i]);
      //remember the slice
      _slices.push_back(slice);
    }
  }
  cout << "Number of slices: " << _slices.size() << endl;

  for (int i = 0; i < _slices.size(); i++) {
    _bias[i].Initialize(_slices[i].GetImageAttributes());
    _weights[i].Initialize(_slices[i].GetImageAttributes());
  }
}

void irtkReconstruction::SetSlicesAndTransformations(vector<irtkRealImage>& slices,
  vector<irtkRigidTransformation>& slice_transformations,
  vector<int>& stack_ids,
  vector<double>& thickness)
{
  _slices.clear();
  _stack_index.clear();
  _transformations.clear();
  _transformations_gpu.clear();
  _slices.clear();
  _simulated_slices.clear();
  _simulated_weights.clear();
  _simulated_inside.clear();

  //for each slice
  for (unsigned int i = 0; i < slices.size(); i++) {
    //get slice
    irtkRealImage slice = slices[i];
    //std::cout << "setting slice " << i << "\n";
    //slice.Print();
    //set correct voxel size in the stack. Z size is equal to slice thickness.
    slice.PutPixelSize(slice.GetXSize(), slice.GetYSize(), thickness[i]);
    //remember the slice
    _slices.push_back(slice);
    _simulated_slices.push_back(slice);
    _simulated_weights.push_back(slice);
    _simulated_inside.push_back(slice);
    //remember stack index for this slice
    _stack_index.push_back(stack_ids[i]);
    //get slice transformation
    _transformations.push_back(slice_transformations[i]);
    _transformations_gpu.push_back(slice_transformations[i]);
  }
}

void irtkReconstruction::UpdateSlices(vector<irtkRealImage>& stacks, vector<double>& thickness)
{
  _slices.clear();
  //for each stack
  for (unsigned int i = 0; i < stacks.size(); i++) {
    //image attributes contain image and voxel size
    irtkImageAttributes attr = stacks[i].GetImageAttributes();

    //attr._z is number of slices in the stack
    for (int j = 0; j < attr._z; j++) {
      //create slice by selecting the appropreate region of the stack
      irtkRealImage slice = stacks[i].GetRegion(0, 0, j, attr._x, attr._y, j + 1);
      //set correct voxel size in the stack. Z size is equal to slice thickness.
      slice.PutPixelSize(attr._dx, attr._dy, thickness[i]);
      //remember the slice
      _slices.push_back(slice);
    }
  }
  cout << "Number of slices: " << _slices.size() << endl;

}

void irtkReconstruction::MaskSlices()
{
  cout << "Masking slices ... ";

  double x, y, z;
  int i, j;

  //Check whether we have a mask
  if (!_have_mask) {
    cout << "Could not mask slices because no mask has been set." << endl;
    return;
  }
  printf("%d %d \n", _slices.size(), _transformations.size());
  //_mask.Write("mask.nii");

  //mask slices
  for (int unsigned inputIndex = 0; inputIndex < _slices.size(); inputIndex++) {
    irtkRealImage& slice = _slices[inputIndex];
    for (i = 0; i < slice.GetX(); i++)
      for (j = 0; j < slice.GetY(); j++) {
      //if the value is smaller than 1 assume it is padding
      if (slice(i, j, 0) < 0.01)
        slice(i, j, 0) = -1;
      //image coordinates of a slice voxel
      x = i;
      y = j;
      z = 0;
      //change to world coordinates in slice space
      slice.ImageToWorld(x, y, z);
      //world coordinates in volume space
      _transformations[inputIndex].Transform(x, y, z);
      //image coordinates in volume space
      _mask.WorldToImage(x, y, z);
      x = round(x);
      y = round(y);
      z = round(z);
      //if the voxel is outside mask ROI set it to -1 (padding value)
      if ((x >= 0) && (x < _mask.GetX()) && (y >= 0) && (y < _mask.GetY()) && (z >= 0)
        && (z < _mask.GetZ())) {
        if (_mask(x, y, z) == 0)
          slice(i, j, 0) = -1;
      }
      else
        slice(i, j, 0) = -1;
      }
  }

  cout << "done." << endl;
}


//TODO implement non rigid registration and its evaluation in cuda...
class ParallelSliceToVolumeRegistration {
public:
  irtkReconstruction *reconstructor;

  ParallelSliceToVolumeRegistration(irtkReconstruction *_reconstructor) :
    reconstructor(_reconstructor) { }

  void operator() (const blocked_range<size_t> &r) const {

    irtkImageAttributes attr = reconstructor->_reconstructed.GetImageAttributes();

    for (size_t inputIndex = r.begin(); inputIndex != r.end(); ++inputIndex) {

      irtkImageRigidRegistrationWithPadding registration;
      irtkGreyPixel smin, smax;
      irtkGreyImage target;
      irtkRealImage slice, w, b, t;
      irtkResamplingWithPadding<irtkRealPixel> resampling(attr._dx, attr._dx, attr._dx, -1);
      // irtkReconstruction dummy_reconstruction; // this also creats an unwanted instance of the GPU reconstruction

      //target = _slices[inputIndex];
      t = reconstructor->_slices[inputIndex];
      resampling.SetInput(&reconstructor->_slices[inputIndex]);
      resampling.SetOutput(&t);
      resampling.Run();
      target = t;

      target.GetMinMax(&smin, &smax);

      if (smax > -1) {
        //put origin to zero
        irtkRigidTransformation offset;
        //dummy_reconstruction.ResetOrigin(target,offset);
        irtkReconstruction::ResetOrigin(target, offset);
        irtkMatrix mo = offset.GetMatrix();
        irtkMatrix m = reconstructor->_transformations[inputIndex].GetMatrix();
        m = m*mo;
        reconstructor->_transformations[inputIndex].PutMatrix(m);
        //std::cout << " ofsMatrix: " << inputIndex << std::endl;
        //reconstructor->_transformations[inputIndex].GetMatrix().Print();

        irtkGreyImage source = reconstructor->_reconstructed;
        registration.SetInput(&target, &source);
        registration.SetOutput(&reconstructor->_transformations[inputIndex]);
        registration.GuessParameterSliceToVolume();
        registration.SetTargetPadding(-1);
        registration.Run();

        reconstructor->_slices_regCertainty[inputIndex] = registration.last_similarity;
        //undo the offset
        mo.Invert();
        m = reconstructor->_transformations[inputIndex].GetMatrix();
        m = m*mo;
        reconstructor->_transformations[inputIndex].PutMatrix(m);
      }

      printf(".");
    }
  }

  // execute
  void operator() () const {
    task_scheduler_init init(tbb_no_threads);
    parallel_for(blocked_range<size_t>(0, reconstructor->_slices.size()),
      *this);
    init.terminate();
  }

};


void irtkReconstruction::testCPURegGPU()
{
  if (_debugGPU)
  {
    std::vector<Matrix4> transf;
    for (int i = 0; i < _transformations_gpu.size(); i++)
    {
      transf.push_back(toMatrix4(_transformations_gpu[i].GetMatrix()));
    }

    reconstructionGPU->testCPUReg(transf);
    cudaDeviceSynchronize();

    irtkGenericImage<float> regTest(reconstructionGPU->regSlices.size.x, reconstructionGPU->regSlices.size.y, reconstructionGPU->regSlices.size.z);
    reconstructionGPU->debugRegSlicesVolume(regTest.GetPointerToVoxels());
    regTest.Write("regTestCPU.nii");
  }

}


void irtkReconstruction::PrepareRegistrationSlices()
{
  irtkImageAttributes attr = _reconstructed.GetImageAttributes();
  irtkResamplingWithPadding<irtkRealPixel> resampling(attr._dx, attr._dx, attr._dx, -1);

  vector<Matrix4> slices_resampledI2W;
  for (int inputIndex = 0; inputIndex < _slices.size(); inputIndex++)
  {
    irtkRealImage t = _slices[inputIndex];
    resampling.SetInput(&_slices[inputIndex]);
    resampling.SetOutput(&t);
    resampling.Run();
    _slices_resampled.push_back(t);
    slices_resampledI2W.push_back(toMatrix4(t.GetImageToWorldMatrix()));
  }


  uint3 reqVDims = make_uint3(INT_MIN, INT_MIN, 0);
  uint3 wasteVDims = make_uint3(INT_MAX, INT_MAX, 0);

  for (int i = 0; i < _slices_resampled.size(); i++)
  {
    reqVDims = make_uint3(max((int)reqVDims.x, _slices_resampled[i].GetX()), max((int)reqVDims.y, _slices_resampled[i].GetY()), _slices_resampled.size());
    wasteVDims = make_uint3(min((int)wasteVDims.x, _slices_resampled[i].GetX()), min((int)wasteVDims.y, _slices_resampled[i].GetY()), _slices_resampled.size());
  }

  double waste = (((reqVDims.x - wasteVDims.x) * (reqVDims.y - wasteVDims.y) * _slices_resampled.size())*sizeof(double)*5.0) / 1024.0;
  printf("GPU memory waste approx RegSlices: %f KB with %d %d %d %d\n", waste, reqVDims.x, reqVDims.y, wasteVDims.x, wasteVDims.y);

  irtkGenericImage<float> combinedStacks(reqVDims.x, reqVDims.y, reqVDims.z);
  float* ptr = combinedStacks.GetPointerToVoxels();

  //all the same dimensions
  reconstructionGPU->initRegStorageVolumes(reqVDims, make_float3(_slices_resampled[0].GetXSize(), _slices_resampled[0].GetYSize(), _slices_resampled[0].GetZSize()));
  vector<int> sizesX;
  vector<int> sizesY;
  std::vector<float3> slice_dims;
  combinedStacks = -1.0;

  for (int i = 0; i < _slices_resampled.size(); i++)
  {
    irtkRealImage slice = _slices_resampled[i];
    //We need to do this line wise because of different cropping sizes
    for (int y = 0; y < slice.GetY(); y++)
    {
      for (int x = 0; x < slice.GetX(); x++)
      {
        combinedStacks(x, y, i) = slice(x, y, 0);
      }
      ptr += combinedStacks.GetX();
    }
    ptr += abs(combinedStacks.GetY() - slice.GetY())*combinedStacks.GetX();
    //TODO - set slice sizes
    sizesX.push_back(slice.GetX());
    sizesY.push_back(slice.GetY());
    slice_dims.push_back(make_float3(_slices_resampled[i].GetXSize(), _slices_resampled[i].GetYSize(), _slices_resampled[i].GetZSize()));
  }
  //combinedStacks.Write("combinedStacks.nii");

  reconstructionGPU->FillRegSlices(combinedStacks.GetPointerToVoxels(), slices_resampledI2W);
  //std::cin.get();
  //reconstructionGPU->setSliceDims(slice_dims, _quality_factor);
  //reconstructionGPU->reconstructedVoxelSize = _reconstructed.GetXSize();
  //reconstructionGPU->UpdateScaleVector(_scale_gpu, _slice_weight_gpu);


  if (_debugGPU)
  {
    irtkGenericImage<float> debugSlices(reqVDims.x, reqVDims.y, reqVDims.z);
    reconstructionGPU->getRegSlicesVol_debug(debugSlices.GetPointerToVoxels());
    debugSlices.Write("debugRegistrationSlices.nii");
    cudaDeviceSynchronize();
  }

}
class ParallelSliceToVolumeRegistrationGPU {
public:
  irtkReconstruction *reconstructor;

  ParallelSliceToVolumeRegistrationGPU(irtkReconstruction *_reconstructor) :
    reconstructor(_reconstructor) { }

  void operator() (const blocked_range<size_t> &r) const {

    irtkImageAttributes attr = reconstructor->_reconstructed.GetImageAttributes();

    unsigned int devslice = 0;
    for (size_t inputIndex = r.begin(); inputIndex != r.end(); ++inputIndex) {

      //TODO poll until GPU is free, then do
      //TODO get current thread number, assign to stream
      //execute registration for single slice with streams
      int alls = reconstructor->_slices.size();
      int curid = (int)inputIndex%reconstructor->reconstructionGPU->devicesToUse.size(); //TODO improve. find number of CPUs
      printf("current proc ID %d dev_slice %d inputIndex %d\n", curid, devslice, inputIndex);
      printf("ERROR NOT WORKING ANYMORE\n");
      exit(-4646);
      //reconstructor->reconstructionGPU->registerSliceToVolume(reconstructor->_transf[inputIndex], inputIndex, devslice, curid);
      devslice++;
    }
  }

  // execute
  void operator() () const {
    task_scheduler_init init(reconstructor->reconstructionGPU->devicesToUse.size());
    //tbb_no_threads
    parallel_for(blocked_range<size_t>(0, reconstructor->_slices.size()),
      *this);
    init.terminate();
  }

};

void irtkReconstruction::SliceToVolumeRegistrationGPU()
{
  if (_debug)
    cout << "SliceToVolumeRegistration" << endl;

  std::vector<irtkMatrix> mos;
  std::vector<double3> oofs;
  std::vector<Matrix4> ofsSlice;
  for (int i = 0; i < _slices_resampled.size(); i++)
  {
    irtkRigidTransformation offset;
    //dummy_reconstruction.ResetOrigin(target,offset);
    double ox, oy, oz;
    _slices_resampled[i].GetOrigin(ox, oy, oz);
    oofs.push_back(make_double3(ox, oy, oz));
    _slices_resampled[i].PutOrigin(0, 0, 0);
    offset.PutTranslationX(ox);
    offset.PutTranslationY(oy);
    offset.PutTranslationZ(oz);
    offset.PutRotationX(0);
    offset.PutRotationY(0);
    offset.PutRotationZ(0);

    irtkMatrix mo = offset.GetMatrix();
    irtkMatrix m = _transformations_gpu[i].GetMatrix();
    m = m*mo;
    mos.push_back(mo);
    _transf.push_back(toMatrix4(m));
    ofsSlice.push_back(toMatrix4(_slices_resampled[i].GetImageToWorldMatrix()));
  }
  reconstructionGPU->updateResampledSlicesI2W(ofsSlice);

  reconstructionGPU->prepareSliceToVolumeReg();
  //slightly different results. investigate this
#if 0
  ParallelSliceToVolumeRegistrationGPU registration(this);
  registration();
#else
  reconstructionGPU->registerSlicesToVolume(_transf);
#endif
  //cudaDeviceSynchronize();

  //TODO update sliceTransforms_, Matrix4 to irtkTransform
  for (int i = 0; i < _transformations_gpu.size(); i++)
  {
    irtkMatrix mat = fromMatrix4(_transf[i]);
    irtkMatrix mo = mos[i];
    mo.Invert();
    mat = mat*mo;
    _transformations_gpu[i].PutMatrix(mat);

    double3 oofs_ = oofs[i];
    _slices_resampled[i].PutOrigin(oofs_.x, oofs_.y, oofs_.z);
  }

  for (int i = 0; i < _transformations.size(); i++)
  {
    std::cout << "CPU: ";
    _transformations[i].Print();
    std::cout << "GPU: ";
    _transformations_gpu[i].Print();
  }

  if (_debugGPU)
  {
    irtkGenericImage<float> regTest(reconstructionGPU->regSlices.size.x, reconstructionGPU->regSlices.size.y, reconstructionGPU->regSlices.size.z);
    reconstructionGPU->debugRegSlicesVolume(regTest.GetPointerToVoxels());
    regTest.Write("regTestGPU.nii");
}

}


void irtkReconstruction::SliceToVolumeRegistration()
{
  if (_slices_regCertainty.size() == 0) _slices_regCertainty.resize(_slices.size());
  if (_debug)
    cout << "SliceToVolumeRegistration" << endl;
  ParallelSliceToVolumeRegistration registration(this);
  registration();
  if (_useCPUReg)
  {
    _transformations_gpu = _transformations;
  }
  printf("\n");
}

class ParallelCoeffInit {
public:
  irtkReconstruction *reconstructor;

  ParallelCoeffInit(irtkReconstruction *_reconstructor) :
    reconstructor(_reconstructor) { }

  void operator() (const blocked_range<size_t> &r) const {

    for (size_t inputIndex = r.begin(); inputIndex != r.end(); ++inputIndex) {

      bool slice_inside;

      //current slice
      //irtkRealImage slice;

      //get resolution of the volume
      double vx, vy, vz;
      reconstructor->_reconstructed.GetPixelSize(&vx, &vy, &vz);
      //volume is always isotropic
      double res = vx;

      //start of a loop for a slice inputIndex
      cout << inputIndex << " ";

      //read the slice
      irtkRealImage& slice = reconstructor->_slices[inputIndex];

      //prepare structures for storage
      POINT3D p;
      VOXELCOEFFS empty;
      SLICECOEFFS slicecoeffs(slice.GetX(), vector < VOXELCOEFFS >(slice.GetY(), empty));

      //to check whether the slice has an overlap with mask ROI
      slice_inside = false;

      //PSF will be calculated in slice space in higher resolution

      //get slice voxel size to define PSF
      double dx, dy, dz;
      slice.GetPixelSize(&dx, &dy, &dz);

      //sigma of 3D Gaussian (sinc with FWHM=dx or dy in-plane, Gaussian with FWHM = dz through-plane)
      double sigmax = 1.2 * dx / 2.3548;
      double sigmay = 1.2 * dy / 2.3548;
      double sigmaz = dz / 2.3548;
      /*
      cout<<"Original sigma"<<sigmax<<" "<<sigmay<<" "<<sigmaz<<endl;

      //readjust for resolution of the volume
      //double sigmax,sigmay,sigmaz;
      double sigmamin = res/(3*2.3548);

      if((dx-res)>sigmamin)
      sigmax = 1.2 * sqrt(dx*dx-res*res) / 2.3548;
      else sigmax = sigmamin;

      if ((dy-res)>sigmamin)
      sigmay = 1.2 * sqrt(dy*dy-res*res) / 2.3548;
      else
      sigmay=sigmamin;
      if ((dz-1.2*res)>sigmamin)
      sigmaz = sqrt(dz*dz-1.2*1.2*res*res) / 2.3548;
      else sigmaz=sigmamin;

      cout<<"Adjusted sigma:"<<sigmax<<" "<<sigmay<<" "<<sigmaz<<endl;
      */

      //calculate discretized PSF

      //isotropic voxel size of PSF - derived from resolution of reconstructed volume
      double size = res / reconstructor->_quality_factor;

      //number of voxels in each direction
      //the ROI is 2*voxel dimension

      int xDim = round(2 * dx / size);
      int yDim = round(2 * dy / size);
      int zDim = round(2 * dz / size);

      //image corresponding to PSF
      irtkImageAttributes attr;
      attr._x = xDim;
      attr._y = yDim;
      attr._z = zDim;
      attr._dx = size;
      attr._dy = size;
      attr._dz = size;
      irtkRealImage PSF(attr);

      //centre of PSF
      double cx, cy, cz;
      cx = 0.5 * (xDim - 1);
      cy = 0.5 * (yDim - 1);
      cz = 0.5 * (zDim - 1);
      PSF.ImageToWorld(cx, cy, cz);

      double x, y, z;
      double sum = 0;
      int i, j, k;
      for (i = 0; i < xDim; i++)
        for (j = 0; j < yDim; j++)
          for (k = 0; k < zDim; k++) {
        x = i;
        y = j;
        z = k;
        PSF.ImageToWorld(x, y, z);
        x -= cx;
        y -= cy;
        z -= cz;
        //continuous PSF does not need to be normalized as discrete will be
        PSF(i, j, k) = exp(
          -x * x / (2 * sigmax * sigmax) - y * y / (2 * sigmay * sigmay)
          - z * z / (2 * sigmaz * sigmaz));
        sum += PSF(i, j, k);
          }
      PSF /= sum;

      if (reconstructor->_debug)
        if (inputIndex == 0)
          PSF.Write("PSF.nii.gz");

      //prepare storage for PSF transformed and resampled to the space of reconstructed volume
      //maximum dim of rotated kernel - the next higher odd integer plus two to accound for rounding error of tx,ty,tz.
      //Note conversion from PSF image coordinates to tPSF image coordinates *size/res
      int dim = (floor(ceil(sqrt(double(xDim * xDim + yDim * yDim + zDim * zDim)) * size / res) / 2))
        * 2 + 1 + 2;
      //prepare image attributes. Voxel dimension will be taken from the reconstructed volume
      attr._x = dim;
      attr._y = dim;
      attr._z = dim;
      attr._dx = res;
      attr._dy = res;
      attr._dz = res;
      //create matrix from transformed PSF
      irtkRealImage tPSF(attr);
      //calculate centre of tPSF in image coordinates
      int centre = (dim - 1) / 2;

      //for each voxel in current slice calculate matrix coefficients
      int ii, jj, kk;
      int tx, ty, tz;
      int nx, ny, nz;
      int l, m, n;
      double weight;
      for (i = 0; i < slice.GetX(); i++)
        for (j = 0; j < slice.GetY(); j++)
          if (slice(i, j, 0) != -1) {
        //calculate centrepoint of slice voxel in volume space (tx,ty,tz)
        x = i;
        y = j;
        z = 0;
        slice.ImageToWorld(x, y, z);
        reconstructor->_transformations[inputIndex].Transform(x, y, z);
        reconstructor->_reconstructed.WorldToImage(x, y, z);
        tx = round(x);
        ty = round(y);
        tz = round(z);

        //Clear the transformed PSF
        for (ii = 0; ii < dim; ii++)
          for (jj = 0; jj < dim; jj++)
            for (kk = 0; kk < dim; kk++)
              tPSF(ii, jj, kk) = 0;

        //for each POINT3D of the PSF
        for (ii = 0; ii < xDim; ii++)
          for (jj = 0; jj < yDim; jj++)
            for (kk = 0; kk < zDim; kk++) {
          //Calculate the position of the POINT3D of
          //PSF centered over current slice voxel                            
          //This is a bit complicated because slices
          //can be oriented in any direction 

          //PSF image coordinates
          x = ii;
          y = jj;
          z = kk;
          //change to PSF world coordinates - now real sizes in mm
          PSF.ImageToWorld(x, y, z);
          //centre around the centrepoint of the PSF
          x -= cx;
          y -= cy;
          z -= cz;

          //Need to convert (x,y,z) to slice image
          //coordinates because slices can have
          //transformations included in them (they are
          //nifti)  and those are not reflected in
          //PSF. In slice image coordinates we are
          //sure that z is through-plane 

          //adjust according to voxel size
          x /= dx;
          y /= dy;
          z /= dz;
          //center over current voxel
          x += i;
          y += j;

          //convert from slice image coordinates to world coordinates
          slice.ImageToWorld(x, y, z);

          //x+=(vx-cx); y+=(vy-cy); z+=(vz-cz);
          //Transform to space of reconstructed volume
          reconstructor->_transformations[inputIndex].Transform(x, y, z);
          //Change to image coordinates
          reconstructor->_reconstructed.WorldToImage(x, y, z);

          //determine coefficients of volume voxels for position x,y,z
          //using linear interpolation

          //Find the 8 closest volume voxels

          //lowest corner of the cube
          nx = (int)floor(x);
          ny = (int)floor(y);
          nz = (int)floor(z);

          //not all neighbours might be in ROI, thus we need to normalize
          //(l,m,n) are image coordinates of 8 neighbours in volume space
          //for each we check whether it is in volume
          sum = 0;
          //to find wether the current slice voxel has overlap with ROI
          bool inside = false;
          for (l = nx; l <= nx + 1; l++)
            if ((l >= 0) && (l < reconstructor->_reconstructed.GetX()))
              for (m = ny; m <= ny + 1; m++)
                if ((m >= 0) && (m < reconstructor->_reconstructed.GetY()))
                  for (n = nz; n <= nz + 1; n++)
                    if ((n >= 0) && (n < reconstructor->_reconstructed.GetZ())) {
            weight = (1 - fabs(l - x)) * (1 - fabs(m - y)) * (1 - fabs(n - z));
            sum += weight;
            if (reconstructor->_mask(l, m, n) == 1) {
              inside = true;
              slice_inside = true;
            }
                    }
          //if there were no voxels do nothing
          if ((sum <= 0) || (!inside))
            continue;
          //now calculate the transformed PSF
          for (l = nx; l <= nx + 1; l++)
            if ((l >= 0) && (l < reconstructor->_reconstructed.GetX()))
              for (m = ny; m <= ny + 1; m++)
                if ((m >= 0) && (m < reconstructor->_reconstructed.GetY()))
                  for (n = nz; n <= nz + 1; n++)
                    if ((n >= 0) && (n < reconstructor->_reconstructed.GetZ())) {
            weight = (1 - fabs(l - x)) * (1 - fabs(m - y)) * (1 - fabs(n - z));

            //image coordinates in tPSF
            //(centre,centre,centre) in tPSF is aligned with (tx,ty,tz)
            int aa, bb, cc;
            aa = l - tx + centre;
            bb = m - ty + centre;
            cc = n - tz + centre;

            //resulting value
            double value = PSF(ii, jj, kk) * weight / sum;

            //Check that we are in tPSF
            if ((aa < 0) || (aa >= dim) || (bb < 0) || (bb >= dim) || (cc < 0)
              || (cc >= dim)) {
              cerr << "Error while trying to populate tPSF. " << aa << " " << bb
                << " " << cc << endl;
              cerr << l << " " << m << " " << n << endl;
              cerr << tx << " " << ty << " " << tz << endl;
              cerr << centre << endl;
              tPSF.Write("tPSF.nii");
              exit(1);
            }
            else
              //update transformed PSF
              tPSF(aa, bb, cc) += value;
                    }
            } //end of the loop for PSF points

        //store tPSF values
        for (ii = 0; ii < dim; ii++)
          for (jj = 0; jj < dim; jj++)
            for (kk = 0; kk < dim; kk++)
              if (tPSF(ii, jj, kk) > 0) {
          p.x = ii + tx - centre;
          p.y = jj + ty - centre;
          p.z = kk + tz - centre;
          p.value = tPSF(ii, jj, kk);
          slicecoeffs[i][j].push_back(p);
              }
        //cout << " n = " << slicecoeffs[i][j].size() << std::endl;
          } //end of loop for slice voxels

      //tPSF.Write("tPSF.nii");
      //PSF.Write("PSF.nii");


      reconstructor->_volcoeffs[inputIndex] = slicecoeffs;
      reconstructor->_slice_inside_cpu[inputIndex] = slice_inside;

    }  //end of loop through the slices                            

  }

  // execute
  void operator() () const {
    task_scheduler_init init(tbb_no_threads);
    parallel_for(blocked_range<size_t>(0, reconstructor->_slices.size()),
      *this);
    init.terminate();
  }

};

void irtkReconstruction::CoeffInit()
{
  if (_debug)
    cout << "CoeffInit" << endl;

  //clear slice-volume matrix from previous iteration
  _volcoeffs.clear();
  _volcoeffs.resize(_slices.size());

  //clear indicator of slice having and overlap with volumetric mask
  _slice_inside_cpu.clear();
  _slice_inside_cpu.resize(_slices.size());

  cout << "Initialising matrix coefficients...";
  ParallelCoeffInit coeffinit(this);
  coeffinit();
  cout << " ... done." << endl;

  //prepare image for volume weights, will be needed for Gaussian Reconstruction
  _volume_weights.Initialize(_reconstructed.GetImageAttributes());
  _volume_weights = 0;

  int inputIndex, i, j, n, k;
  POINT3D p;
  for (inputIndex = 0; inputIndex < _slices.size(); ++inputIndex) {
    for (i = 0; i < _slices[inputIndex].GetX(); i++)
      for (j = 0; j < _slices[inputIndex].GetY(); j++) {
      n = _volcoeffs[inputIndex][i][j].size();
      for (k = 0; k < n; k++) {
        p = _volcoeffs[inputIndex][i][j][k];
        _volume_weights(p.x, p.y, p.z) += p.value;
      }
      }
  }
  if (_debug || _debugGPU)
    _volume_weights.Write("volume_weightsCPU.nii");

  //find average volume weight to modify alpha parameters accordingly
  irtkRealPixel *ptr = _volume_weights.GetPointerToVoxels();
  irtkRealPixel *pm = _mask.GetPointerToVoxels();
  double sum = 0;
  int num = 0;
  for (int i = 0; i < _volume_weights.GetNumberOfVoxels(); i++) {
    if (*pm == 1) {
      sum += *ptr;
      num++;
    }
    ptr++;
    pm++;
  }
  _average_volume_weight = sum / num;

  if (_debug) {
    cout << "Average volume weight is " << _average_volume_weight << endl;
  }

}  //end of CoeffInit()

void irtkReconstruction::SyncCPU()
{
  irtkGenericImage<float> trecon = _reconstructed_gpu;


  reconstructionGPU->syncCPU(trecon.GetPointerToVoxels());
  _reconstructed = trecon;
  printf("Sync done. ready for more...\n");
}

irtkRealImage irtkReconstruction::GetReconstructedGPU()
{
  irtkGenericImage<float> trecon = _reconstructed_gpu;

  //debug only
  reconstructionGPU->syncCPU(trecon.GetPointerToVoxels());

  return trecon;
}

void irtkReconstruction::GaussianReconstructionGPU()
{

  cout << "Gaussian reconstruction ... ";
  vector<int> voxel_num_gpu;
  reconstructionGPU->GaussianReconstruction(voxel_num_gpu);
  //irtkRealImage trecon = _reconstructed;
  //debug only
  //reconstructionGPU->syncCPU(trecon.GetPointerToVoxels());
  //trecon.Write("GaussianRecon_Test2.nii");

  int slice_vox_num;
  cout << "done." << endl;

  if (_debug)
    _reconstructed.Write("init.nii.gz");

  //now find slices with small overlap with ROI and exclude them.

  vector<int> voxel_num_tmp;
  for (int i = 0; i < voxel_num_gpu.size(); i++)
    voxel_num_tmp.push_back(voxel_num_gpu[i]);

  //find median
  sort(voxel_num_tmp.begin(), voxel_num_tmp.end());
  int median = voxel_num_tmp[round(voxel_num_tmp.size()*0.5)];

  //remember slices with small overlap with ROI
  _small_slices.clear();
  for (int i = 0; i < voxel_num_gpu.size(); i++)
    if (voxel_num_gpu[i] < 0.1*median)
      _small_slices.push_back(i);

  if (_debug || _debugGPU) {
    cout << "Small slices GPU:";
    for (int i = 0; i < _small_slices.size(); i++)
      cout << " " << _small_slices[i];
    cout << endl;
  }

  if (_debugGPU)
  {
    irtkGenericImage<float> bweights;
    bweights.Initialize(_reconstructed.GetImageAttributes());
    //irtkRealImage bweights(reconstructionGPU->reconstructed_volWeigths.size.x, reconstructionGPU->reconstructed_volWeigths.size.y, reconstructionGPU->reconstructed_volWeigths.size.z);
    reconstructionGPU->combineWeights(bweights.GetPointerToVoxels());
    bweights.Write("volume_weightsGPU.nii");

    irtkGenericImage<float> weightnorm(reconstructionGPU->v_PSF_sums_.size.x, reconstructionGPU->v_PSF_sums_.size.y, reconstructionGPU->v_PSF_sums_.size.z);
    reconstructionGPU->debugv_PSF_sums(weightnorm.GetPointerToVoxels());
    weightnorm.Write("v_PSF_sums_.nii");
  }

  //registration test
#if 0
  //TODO improve and copy transformations back
  //finally, leave on device, init slice trans only for init once
  std::vector<Matrix4> transf;
  reconstructionGPU->registerSlicesToVolume(transf);
  irtkGenericImage<float> regTest(reconstructionGPU->regSlices.size.x, reconstructionGPU->regSlices.size.y, reconstructionGPU->regSlices.size.z);
  reconstructionGPU->debugRegSlicesVolume(regTest.GetPointerToVoxels());
  regTest.Write("regTest.nii");

  //printf("second test\n");
  //reconstructionGPU->registerSlicesToVolume();
#endif

}


void irtkReconstruction::GaussianReconstruction()
{
  //vector<int> voxel_num_;  
  //reconstructionGPU->GaussianReconstruction(voxel_num_);

  cout << "Gaussian reconstruction ... ";
  unsigned int inputIndex;
  int i, j, k, n;
  irtkRealImage slice;
  double scale;
  POINT3D p;
  vector<int> voxel_num;
  int slice_vox_num;

  //clear _reconstructed image
  _reconstructed = 0;

  //std::cout << "voxel_num CPU: ";
  //CPU
  for (inputIndex = 0; inputIndex < _slices.size(); ++inputIndex) {
    //copy the current slice
    slice = _slices[inputIndex];
    //alias the current bias image
    irtkRealImage& b = _bias[inputIndex];
    //read current scale factor
    scale = _scale_cpu[inputIndex];

    slice_vox_num = 0;

    //Distribute slice intensities to the volume
    for (i = 0; i < slice.GetX(); i++)
      for (j = 0; j < slice.GetY(); j++)
        if (slice(i, j, 0) != -1) {
      //biascorrect and scale the slice
      slice(i, j, 0) *= exp(-b(i, j, 0)) * scale;

      //number of volume voxels with non-zero coefficients
      //for current slice voxel
      n = _volcoeffs[inputIndex][i][j].size();

      //if given voxel is not present in reconstructed volume at all,
      //pad it

      //if (n == 0)
      //_slices[inputIndex].PutAsDouble(i, j, 0, -1);
      //calculate num of vox in a slice that have overlap with roi
      if (n > 0)
        slice_vox_num++;

      //add contribution of current slice voxel to all voxel volumes
      //to which it contributes
      for (k = 0; k < n; k++) {
        p = _volcoeffs[inputIndex][i][j][k];
        _reconstructed(p.x, p.y, p.z) += p.value * slice(i, j, 0);
      }
      //debug
      //p = _volcoeffs[inputIndex][i][j][0];
      //_reconstructed(p.x, p.y, p.z) += slice(i, j, 0);
        }
    voxel_num.push_back(slice_vox_num);
    //std::cout << voxel_num[inputIndex] << " ";
    //end of loop for a slice inputIndex
  }

  //normalize the volume by proportion of contributing slice voxels
  //for each volume voxe
  _reconstructed /= _volume_weights;

  cout << "done." << endl;

  if (_debug)
    _reconstructed.Write("init.nii.gz");

  //now find slices with small overlap with ROI and exclude them.

  vector<int> voxel_num_tmp;
  for (i = 0; i < voxel_num.size(); i++)
    voxel_num_tmp.push_back(voxel_num[i]);

  //find median
  sort(voxel_num_tmp.begin(), voxel_num_tmp.end());
  int median = voxel_num_tmp[round(voxel_num_tmp.size()*0.5)];

  //remember slices with small overlap with ROI
  _small_slices.clear();
  for (i = 0; i < voxel_num.size(); i++)
    if (voxel_num[i] < 0.1*median)
      _small_slices.push_back(i);

  if (_debug || _debugGPU) {
    cout << "Small slices CPU:";
    for (i = 0; i < _small_slices.size(); i++)
      cout << " " << _small_slices[i];
    cout << endl;
  }
}

void irtkReconstruction::InitializeEM()
{
  if (_debug)
    cout << "InitializeEM" << endl;

  _weights.clear();
  _bias.clear();
  _scale_cpu.clear();
  _slice_weight_cpu.clear();

  for (unsigned int i = 0; i < _slices.size(); i++) {
    //Create images for voxel weights and bias fields
    _weights.push_back(_slices[i]);
    _bias.push_back(_slices[i]);

    //Create and initialize scales
    _scale_cpu.push_back(1);
    //_scale_gpu.push_back(1);

    //Create and initialize slice weights
    _slice_weight_cpu.push_back(1);
    //_slice_weight_gpu.push_back(1);
  }

  //TODO CUDA
  //Find the range of intensities
  _max_intensity = voxel_limits<irtkRealPixel>::min();
  _min_intensity = voxel_limits<irtkRealPixel>::max();
  for (unsigned int i = 0; i < _slices.size(); i++) {
    //to update minimum we need to exclude padding value
    irtkRealPixel *ptr = _slices[i].GetPointerToVoxels();
    for (int ind = 0; ind < _slices[i].GetNumberOfVoxels(); ind++) {
      if (*ptr > 0) {
        if (*ptr > _max_intensity)
          _max_intensity = *ptr;
        if (*ptr < _min_intensity)
          _min_intensity = *ptr;
      }
      ptr++;
    }
  }
}

void irtkReconstruction::InitializeEMValuesGPU()
{
  if (_debug)
    cout << "InitializeEMValues" << endl;

  _scale_gpu.clear();
  _slice_weight_gpu.clear();

  _slice_weight_gpu.assign(_slices.size(), 1);
  _scale_gpu.assign(_slices.size(), 1);
  reconstructionGPU->UpdateScaleVector(_scale_gpu, _slice_weight_gpu);

  reconstructionGPU->InitializeEMValues();

}

void irtkReconstruction::InitializeEMGPU()
{
  if (_debug)
    cout << "InitializeEM" << endl;

  _scale_gpu.clear();
  _slice_weight_gpu.clear();

  _slice_weight_gpu.assign(_slices.size(), 1);
  _scale_gpu.assign(_slices.size(), 1);
  reconstructionGPU->UpdateScaleVector(_scale_gpu, _slice_weight_gpu);

  reconstructionGPU->InitializeEMValues();

  //TODO CUDA
  //Find the range of intensities
  _max_intensity = voxel_limits<irtkRealPixel>::min();
  _min_intensity = voxel_limits<irtkRealPixel>::max();
  for (unsigned int i = 0; i < _slices.size(); i++) {
    //to update minimum we need to exclude padding value
    irtkRealPixel *ptr = _slices[i].GetPointerToVoxels();
    for (int ind = 0; ind < _slices[i].GetNumberOfVoxels(); ind++) {
      if (*ptr > 0) {
        if (*ptr > _max_intensity)
          _max_intensity = *ptr;
        if (*ptr < _min_intensity)
          _min_intensity = *ptr;
      }
      ptr++;
    }
  }

}

void irtkReconstruction::InitializeEMValues()
{
  if (_debug)
    cout << "InitializeEMValues" << endl;

  for (unsigned int i = 0; i < _slices.size(); i++) {
    //Initialise voxel weights and bias values
    irtkRealPixel *pw = _weights[i].GetPointerToVoxels();
    irtkRealPixel *pb = _bias[i].GetPointerToVoxels();
    irtkRealPixel *pi = _slices[i].GetPointerToVoxels();
    for (int j = 0; j < _weights[i].GetNumberOfVoxels(); j++) {
      if (*pi != -1) {
        *pw = 1;
        *pb = 0;
      }
      else {
        *pw = 0;
        *pb = 0;
      }
      pi++;
      pw++;
      pb++;
    }

    //Initialise slice weights
    _slice_weight_cpu[i] = 1;

    //Initialise scaling factors for intensity matching
    _scale_cpu[i] = 1;
  }
}


void irtkReconstruction::InitializeRobustStatisticsGPU()
{
  if (_debug)
    cout << "InitializeRobustStatistics" << endl;

  reconstructionGPU->InitializeRobustStatistics(_sigma_gpu);

  for (unsigned int inputIndex = 0; inputIndex < _slices.size(); inputIndex++) {
    //if slice does not have an overlap with ROI, set its weight to zero
    if (!_slice_inside_gpu[inputIndex])
      _slice_weight_gpu[inputIndex] = 0;
  }
  //Force exclusion of slices predefined by user
  for (unsigned int i = 0; i < _force_excluded.size(); i++)
    _slice_weight_gpu[_force_excluded[i]] = 0;


  //initialize sigma for slice-wise robust statistics
  _sigma_s_gpu = 0.025;
  //initialize mixing proportion for inlier class in voxel-wise robust statistics
  _mix_gpu = 0.9;
  //initialize mixing proportion for outlier class in slice-wise robust statistics
  _mix_s_gpu = 0.9;
  //Initialise value for uniform distribution according to the range of intensities
  _m_gpu = 1 / (2.1 * _max_intensity - 1.9 * _min_intensity);

  if (_debug || _debugGPU)
    cout << "Initializing robust statistics GPU: " << "sigma=" << sqrt(_sigma_gpu) << " " << "m=" << _m_gpu
    << " " << "mix=" << _mix_gpu << " " << "mix_s=" << _mix_s_gpu << endl;

  reconstructionGPU->UpdateScaleVector(_scale_gpu, _slice_weight_gpu);

}

void irtkReconstruction::InitializeRobustStatistics()
{
  if (_debug)
    cout << "InitializeRobustStatistics" << endl;

  //Initialise parameter of EM robust statistics
  int i, j;
  irtkRealImage slice, sim;
  double sigma = 0;
  int num = 0;

  //for each slice
  for (unsigned int inputIndex = 0; inputIndex < _slices.size(); inputIndex++) {
    slice = _slices[inputIndex];

    //Voxel-wise sigma will be set to stdev of volumetric errors
    //For each slice voxel
    for (i = 0; i < slice.GetX(); i++)
      for (j = 0; j < slice.GetY(); j++)
        if (slice(i, j, 0) != -1) {
      //calculate stev of the errors
      if ((_simulated_inside[inputIndex](i, j, 0) == 1)
        && (_simulated_weights[inputIndex](i, j, 0) > 0.99)) {
        slice(i, j, 0) -= _simulated_slices[inputIndex](i, j, 0);
        sigma += slice(i, j, 0) * slice(i, j, 0);
        num++;
      }
        }

    //if slice does not have an overlap with ROI, set its weight to zero
    if (!_slice_inside_cpu[inputIndex])
      _slice_weight_cpu[inputIndex] = 0;
  }

  //Force exclusion of slices predefined by user
  for (unsigned int i = 0; i < _force_excluded.size(); i++)
    _slice_weight_cpu[_force_excluded[i]] = 0;

  //initialize sigma for voxelwise robust statistics
  _sigma_cpu = sigma / num;
  //initialize sigma for slice-wise robust statistics
  _sigma_s_cpu = 0.025;
  //initialize mixing proportion for inlier class in voxel-wise robust statistics
  _mix_cpu = 0.9;
  //initialize mixing proportion for outlier class in slice-wise robust statistics
  _mix_s_cpu = 0.9;
  //Initialise value for uniform distribution according to the range of intensities
  _m_cpu = 1 / (2.1 * _max_intensity - 1.9 * _min_intensity);

  if (_debug || _debugGPU)
    cout << "Initializing robust statistics CPU: " << "sigma=" << sqrt(_sigma_cpu) << " " << "m=" << _m_cpu
    << " " << "mix=" << _mix_cpu << " " << "mix_s=" << _mix_s_cpu << endl;
}

class ParallelEStep {
  irtkReconstruction* reconstructor;
  vector<double> &slice_potential;

public:

  void operator()(const blocked_range<size_t>& r) const {
    for (size_t inputIndex = r.begin(); inputIndex < r.end(); ++inputIndex) {
      // read the current slice
      irtkRealImage slice = reconstructor->_slices[inputIndex];

      //read current weight image
      reconstructor->_weights[inputIndex] = 0;

      //alias the current bias image
      irtkRealImage& b = reconstructor->_bias[inputIndex];

      //identify scale factor
      double scale = reconstructor->_scale_cpu[inputIndex];

      double num = 0;
      //Calculate error, voxel weights, and slice potential
      for (int i = 0; i < slice.GetX(); i++)
        for (int j = 0; j < slice.GetY(); j++)
          if (slice(i, j, 0) != -1) {
        //bias correct and scale the slice
        slice(i, j, 0) *= exp(-b(i, j, 0)) * scale;

        //number of volumetric voxels to which
        // current slice voxel contributes
        int n = reconstructor->_volcoeffs[inputIndex][i][j].size();

        // if n == 0, slice voxel has no overlap with volumetric ROI,
        // do not process it

        if ((n>0) &&
          (reconstructor->_simulated_weights[inputIndex](i, j, 0) > 0)) {
          slice(i, j, 0) -= reconstructor->_simulated_slices[inputIndex](i, j, 0);

          //calculate norm and voxel-wise weights

          //Gaussian distribution for inliers (likelihood)
          double g = reconstructor->G(slice(i, j, 0), reconstructor->_sigma_cpu);
          //Uniform distribution for outliers (likelihood)
          double m = reconstructor->M(reconstructor->_m_cpu);

          //voxel_wise posterior
          double weight = g * reconstructor->_mix_cpu / (g *reconstructor->_mix_cpu + m * (1 - reconstructor->_mix_cpu));
          reconstructor->_weights[inputIndex].PutAsDouble(i, j, 0, weight);

          //calculate slice potentials
          if (reconstructor->_simulated_weights[inputIndex](i, j, 0) > 0.99) {
            slice_potential[inputIndex] += (1.0 - weight) * (1.0 - weight);
            num++;
          }
        }
        else
          reconstructor->_weights[inputIndex].PutAsDouble(i, j, 0, 0);
          }

      //evaluate slice potential
      if (num > 0)
      {
        slice_potential[inputIndex] = sqrt(slice_potential[inputIndex] / num);
        //std::cout <<  num  << " ";
      }
      else
        slice_potential[inputIndex] = -1; // slice has no unpadded voxels
    }
  }

  ParallelEStep(irtkReconstruction *reconstructor,
    vector<double> &slice_potential) :
    reconstructor(reconstructor), slice_potential(slice_potential)
  { }

  // execute
  void operator() () const {
    task_scheduler_init init(tbb_no_threads);
    parallel_for(blocked_range<size_t>(0, reconstructor->_slices.size()),
      *this);
    init.terminate();
  }

};

void irtkReconstruction::EStepGPU()
{
  //EStep performs calculation of voxel-wise and slice-wise posteriors (weights)
  if (_debug)
    cout << "EStep: " << endl;

  unsigned int inputIndex;
  vector<float> slice_potential_gpu(_slices.size(), 0);
  reconstructionGPU->EStep(_m_gpu, _sigma_gpu, _mix_gpu, slice_potential_gpu);

  if (_debugGPU)
  {
    irtkGenericImage<float> bweights(reconstructionGPU->v_weights.size.x, reconstructionGPU->v_weights.size.y, reconstructionGPU->v_weights.size.z);
    reconstructionGPU->debugWeights(bweights.GetPointerToVoxels());
    bweights.Write("testweightGPU.nii");
  }

  //can stay on CPU

  //To force-exclude slices predefined by a user, set their potentials to -1
  for (unsigned int i = 0; i < _force_excluded.size(); i++)
    slice_potential_gpu[_force_excluded[i]] = -1;

  //exclude slices identified as having small overlap with ROI, set their potentials to -1
  for (unsigned int i = 0; i < _small_slices.size(); i++)
    slice_potential_gpu[_small_slices[i]] = -1;

  //these are unrealistic scales pointing at misregistration - exclude the corresponding slices
  for (inputIndex = 0; inputIndex < slice_potential_gpu.size(); inputIndex++)
    if ((_scale_gpu[inputIndex] < 0.2) || (_scale_gpu[inputIndex]>5)) {
    slice_potential_gpu[inputIndex] = -1;
    }

  // exclude unrealistic transformations
  /*
  int current_stack = 0;
  int nb_stacks = _stack_index[_stack_index.size()-1];
  double tx,ty,tz,rx,ry,rz, nb;
  for ( int i = 0; i < nb_stacks; i++ ) {
  tx = 0;
  ty = 0;
  tz = 0;
  rx = 0;
  ry = 0;
  rz = 0;
  nb = 0;
  for ( int j = 0; j < _slices.size(); j++ ) {
  if ( _stack_index[j] != i )
  continue;
  if ( slice_potential[j] == -1 )
  continue;
  tx += _transformations[j].GetTranslationX();
  ty += _transformations[j].GetTranslationY();
  tz += _transformations[j].GetTranslationZ();
  rx += _transformations[j].GetRotationX();
  ry += _transformations[j].GetRotationY();
  rz += _transformations[j].GetRotationZ();
  nb++;
  }
  tx /= nb;
  ty /= nb;
  tz /= nb;
  rx /= nb;
  ry /= nb;
  rz /= nb;
  for ( int j = 0; j < _slices.size(); j++ ) {
  if ( _stack_index[j] != i )
  continue;
  if ( slice_potential[j] == -1 )
  continue;
  if ( abs( tx - _transformations[j].GetTranslationX() ) > 20
  || abs( ty - _transformations[j].GetTranslationY() ) > 20
  || abs( tz - _transformations[j].GetTranslationZ() ) > 20
  || abs( rx - _transformations[j].GetRotationX() ) > 5
  || abs( ry - _transformations[j].GetRotationY() ) > 5
  || abs( rz - _transformations[j].GetRotationZ() ) > 5 )
  slice_potential[j] = -1;
  }
  }
  */
  if (_debug || _debugGPU) {
    cout << setprecision(4);
    cout << endl << "Slice potentials GPU: ";
    for (inputIndex = 0; inputIndex < slice_potential_gpu.size(); inputIndex++)
      cout << slice_potential_gpu[inputIndex] << " ";
    cout << endl << "Slice weights GPU: ";
    for (inputIndex = 0; inputIndex < _slice_weight_gpu.size(); inputIndex++)
      cout << _slice_weight_gpu[inputIndex] << " ";
    cout << endl;
  }


  //Calulation of slice-wise robust statistics parameters.
  //This is theoretically M-step,
  //but we want to use latest estimate of slice potentials
  //to update the parameters

  //Calculate means of the inlier and outlier potentials
  double sum = 0, den = 0, sum2 = 0, den2 = 0, maxs = 0, mins = 1;
  for (inputIndex = 0; inputIndex < _slices.size(); inputIndex++)
    if (slice_potential_gpu[inputIndex] >= 0) {
    //calculate means
    sum += slice_potential_gpu[inputIndex] * _slice_weight_gpu[inputIndex];
    den += _slice_weight_gpu[inputIndex];
    sum2 += slice_potential_gpu[inputIndex] * (1.0 - _slice_weight_gpu[inputIndex]);
    den2 += (1.0 - _slice_weight_gpu[inputIndex]);

    //calculate min and max of potentials in case means need to be initalized
    if (slice_potential_gpu[inputIndex] > maxs)
      maxs = slice_potential_gpu[inputIndex];
    if (slice_potential_gpu[inputIndex] < mins)
      mins = slice_potential_gpu[inputIndex];
    }

  if (den > 0)
    _mean_s_gpu = sum / den;
  else
    _mean_s_gpu = mins;

  if (den2 > 0)
    _mean_s2_gpu = sum2 / den2;
  else
    _mean_s2_gpu = (maxs + _mean_s_gpu) / 2.0;

  //Calculate the variances of the potentials
  sum = 0;
  den = 0;
  sum2 = 0;
  den2 = 0;
  for (inputIndex = 0; inputIndex < _slices.size(); inputIndex++)
    if (slice_potential_gpu[inputIndex] >= 0) {
    sum += (slice_potential_gpu[inputIndex] - _mean_s_gpu) * (slice_potential_gpu[inputIndex] - _mean_s_gpu)
      * _slice_weight_gpu[inputIndex];
    den += _slice_weight_gpu[inputIndex];

    sum2 += (slice_potential_gpu[inputIndex] - _mean_s2_gpu) * (slice_potential_gpu[inputIndex] - _mean_s2_gpu)
      * (1 - _slice_weight_gpu[inputIndex]);
    den2 += (1 - _slice_weight_gpu[inputIndex]);

    }

  //_sigma_s
  if ((sum > 0) && (den > 0)) {
    _sigma_s_gpu = sum / den;
    //do not allow too small sigma
    if (_sigma_s_gpu < _step * _step / 6.28)
      _sigma_s_gpu = _step * _step / 6.28;
  }
  else {
    _sigma_s_gpu = 0.025;
    if (_debug) {
      if (sum <= 0)
        cout << "All slices are equal. ";
      if (den < 0) //this should not happen
        cout << "All slices are outliers. ";
      cout << "Setting sigma to " << sqrt(_sigma_s_gpu) << endl;
    }
  }

  //sigma_s2
  if ((sum2 > 0) && (den2 > 0)) {
    _sigma_s2_gpu = sum2 / den2;
    //do not allow too small sigma
    if (_sigma_s2_gpu < _step * _step / 6.28)
      _sigma_s2_gpu = _step * _step / 6.28;
  }
  else {
    _sigma_s2_gpu = (_mean_s2_gpu - _mean_s_gpu) * (_mean_s2_gpu - _mean_s_gpu) / 4;
    //do not allow too small sigma
    if (_sigma_s2_gpu < _step * _step / 6.28)
      _sigma_s2_gpu = _step * _step / 6.28;

    if (_debug) {
      if (sum2 <= 0)
        cout << "All slices are equal. ";
      if (den2 <= 0)
        cout << "All slices inliers. ";
      cout << "Setting sigma_s2 to " << sqrt(_sigma_s2_gpu) << endl;
    }
  }

  //Calculate slice weights
  double gs1, gs2;
  for (inputIndex = 0; inputIndex < _slices.size(); inputIndex++) {
    //Slice does not have any voxels in volumetric ROI
    if (slice_potential_gpu[inputIndex] == -1) {
      _slice_weight_gpu[inputIndex] = 0;
      continue;
    }

    //All slices are outliers or the means are not valid
    if ((den <= 0) || (_mean_s2_gpu <= _mean_s_gpu)) {
      _slice_weight_gpu[inputIndex] = 1;
      continue;
    }

    //likelihood for inliers
    if (slice_potential_gpu[inputIndex] < _mean_s2_gpu)
      gs1 = G(slice_potential_gpu[inputIndex] - _mean_s_gpu, _sigma_s_gpu);
    else
      gs1 = 0;

    //likelihood for outliers
    if (slice_potential_gpu[inputIndex] > _mean_s_gpu)
      gs2 = G(slice_potential_gpu[inputIndex] - _mean_s2_gpu, _sigma_s2_gpu);
    else
      gs2 = 0;

    //calculate slice weight
    double likelihood = gs1 * _mix_s_gpu + gs2 * (1 - _mix_s_gpu);
    if (likelihood > 0)
      _slice_weight_gpu[inputIndex] = gs1 * _mix_s_gpu / likelihood;
    else {
      if (slice_potential_gpu[inputIndex] <= _mean_s_gpu)
        _slice_weight_gpu[inputIndex] = 1;
      if (slice_potential_gpu[inputIndex] >= _mean_s2_gpu)
        _slice_weight_gpu[inputIndex] = 0;
      if ((slice_potential_gpu[inputIndex] < _mean_s2_gpu) && (slice_potential_gpu[inputIndex] > _mean_s_gpu)) //should not happen
        _slice_weight_gpu[inputIndex] = 1;
    }
  }

  //Update _mix_s this should also be part of MStep
  sum = 0;
  int num = 0;
  for (inputIndex = 0; inputIndex < _slices.size(); inputIndex++)
    if (slice_potential_gpu[inputIndex] >= 0) {
    sum += _slice_weight_gpu[inputIndex];
    num++;
    }

  if (num > 0)
    _mix_s_gpu = sum / num;
  else {
    cout << "All slices are outliers. Setting _mix_s to 0.9." << endl;
    _mix_s_gpu = 0.9;
  }

  if (_debug || _debugGPU) {
    cout << setprecision(3);
    cout << "Slice robust statistics parameters GPU: ";
    cout << "means: " << _mean_s_gpu << " " << _mean_s2_gpu << "  ";
    cout << "sigmas: " << sqrt(_sigma_s_gpu) << " " << sqrt(_sigma_s2_gpu) << "  ";
    cout << "proportions: " << _mix_s_gpu << " " << 1 - _mix_s_gpu << endl;
    cout << "Slice weights GPU: ";
    for (inputIndex = 0; inputIndex < _slices.size(); inputIndex++)
      cout << _slice_weight_gpu[inputIndex] << " ";
    /*cout << "Slice potential: ";
    for (inputIndex = 0; inputIndex < _slices.size(); inputIndex++)
    cout << slice_potential[inputIndex] << " ";*/
    cout << endl;
  }

  //TODO only slice weight
  reconstructionGPU->UpdateSliceWeights(_slice_weight_gpu);

}

void irtkReconstruction::EStep()
{
  //EStep performs calculation of voxel-wise and slice-wise posteriors (weights)
  if (_debug)
    cout << "EStep: " << endl;

  unsigned int inputIndex;
  irtkRealImage slice, w, b, sim;
  int num = 0;
  vector<double> slice_potential_cpu(_slices.size(), 0);
  //std::cout << "num Estp CPU: ";
  ParallelEStep parallelEStep(this, slice_potential_cpu);
  parallelEStep();

  if(_debugGPU)
  {
    _weights[40].Write("testweightCPU.nii");
}

  //To force-exclude slices predefined by a user, set their potentials to -1
  for (unsigned int i = 0; i < _force_excluded.size(); i++)
    slice_potential_cpu[_force_excluded[i]] = -1;

  //exclude slices identified as having small overlap with ROI, set their potentials to -1
  for (unsigned int i = 0; i < _small_slices.size(); i++)
    slice_potential_cpu[_small_slices[i]] = -1;

  //these are unrealistic scales pointing at misregistration - exclude the corresponding slices
  for (inputIndex = 0; inputIndex < slice_potential_cpu.size(); inputIndex++)
    if ((_scale_cpu[inputIndex] < 0.2) || (_scale_cpu[inputIndex]>5)) {
    slice_potential_cpu[inputIndex] = -1;
    }

  // exclude unrealistic transformations
  /*
  int current_stack = 0;
  int nb_stacks = _stack_index[_stack_index.size()-1];
  double tx,ty,tz,rx,ry,rz, nb;
  for ( int i = 0; i < nb_stacks; i++ ) {
  tx = 0;
  ty = 0;
  tz = 0;
  rx = 0;
  ry = 0;
  rz = 0;
  nb = 0;
  for ( int j = 0; j < _slices.size(); j++ ) {
  if ( _stack_index[j] != i )
  continue;
  if ( slice_potential[j] == -1 )
  continue;
  tx += _transformations[j].GetTranslationX();
  ty += _transformations[j].GetTranslationY();
  tz += _transformations[j].GetTranslationZ();
  rx += _transformations[j].GetRotationX();
  ry += _transformations[j].GetRotationY();
  rz += _transformations[j].GetRotationZ();
  nb++;
  }
  tx /= nb;
  ty /= nb;
  tz /= nb;
  rx /= nb;
  ry /= nb;
  rz /= nb;
  for ( int j = 0; j < _slices.size(); j++ ) {
  if ( _stack_index[j] != i )
  continue;
  if ( slice_potential[j] == -1 )
  continue;
  if ( abs( tx - _transformations[j].GetTranslationX() ) > 20
  || abs( ty - _transformations[j].GetTranslationY() ) > 20
  || abs( tz - _transformations[j].GetTranslationZ() ) > 20
  || abs( rx - _transformations[j].GetRotationX() ) > 5
  || abs( ry - _transformations[j].GetRotationY() ) > 5
  || abs( rz - _transformations[j].GetRotationZ() ) > 5 )
  slice_potential[j] = -1;
  }
  }
  */
  if (_debug || _debugGPU) {
    cout << setprecision(4);
    cout << endl << "Slice potentials CPU: ";
    for (inputIndex = 0; inputIndex < slice_potential_cpu.size(); inputIndex++)
      cout << slice_potential_cpu[inputIndex] << " ";
    cout << endl << "Slice weights CPU: ";
    for (inputIndex = 0; inputIndex < _slice_weight_cpu.size(); inputIndex++)
      cout << _slice_weight_cpu[inputIndex] << " ";
    cout << endl;
  }


  //Calulation of slice-wise robust statistics parameters.
  //This is theoretically M-step,
  //but we want to use latest estimate of slice potentials
  //to update the parameters

  //Calculate means of the inlier and outlier potentials
  double sum = 0, den = 0, sum2 = 0, den2 = 0, maxs = 0, mins = 1;
  for (inputIndex = 0; inputIndex < _slices.size(); inputIndex++)
    if (slice_potential_cpu[inputIndex] >= 0) {
    //calculate means
    sum += slice_potential_cpu[inputIndex] * _slice_weight_cpu[inputIndex];
    den += _slice_weight_cpu[inputIndex];
    sum2 += slice_potential_cpu[inputIndex] * (1 - _slice_weight_cpu[inputIndex]);
    den2 += (1 - _slice_weight_cpu[inputIndex]);

    //calculate min and max of potentials in case means need to be initalized
    if (slice_potential_cpu[inputIndex] > maxs)
      maxs = slice_potential_cpu[inputIndex];
    if (slice_potential_cpu[inputIndex] < mins)
      mins = slice_potential_cpu[inputIndex];
    }

  if (den > 0)
    _mean_s_cpu = sum / den;
  else
    _mean_s_cpu = mins;

  if (den2 > 0)
    _mean_s2_cpu = sum2 / den2;
  else
    _mean_s2_cpu = (maxs + _mean_s_cpu) / 2;

  //Calculate the variances of the potentials
  sum = 0;
  den = 0;
  sum2 = 0;
  den2 = 0;
  for (inputIndex = 0; inputIndex < _slices.size(); inputIndex++)
    if (slice_potential_cpu[inputIndex] >= 0) {
    sum += (slice_potential_cpu[inputIndex] - _mean_s_cpu) * (slice_potential_cpu[inputIndex] - _mean_s_cpu)
      * _slice_weight_cpu[inputIndex];
    den += _slice_weight_cpu[inputIndex];

    sum2 += (slice_potential_cpu[inputIndex] - _mean_s2_cpu) * (slice_potential_cpu[inputIndex] - _mean_s2_cpu)
      * (1 - _slice_weight_cpu[inputIndex]);
    den2 += (1 - _slice_weight_cpu[inputIndex]);

    }

  //_sigma_s
  if ((sum > 0) && (den > 0)) {
    _sigma_s_cpu = sum / den;
    //do not allow too small sigma
    if (_sigma_s_cpu < _step * _step / 6.28)
      _sigma_s_cpu = _step * _step / 6.28;
  }
  else {
    _sigma_s_cpu = 0.025;
    if (_debug) {
      if (sum <= 0)
        cout << "All slices are equal. ";
      if (den < 0) //this should not happen
        cout << "All slices are outliers. ";
      cout << "Setting sigma to " << sqrt(_sigma_s_cpu) << endl;
    }
  }

  //sigma_s2
  if ((sum2 > 0) && (den2 > 0)) {
    _sigma_s2_cpu = sum2 / den2;
    //do not allow too small sigma
    if (_sigma_s2_cpu < _step * _step / 6.28)
      _sigma_s2_cpu = _step * _step / 6.28;
  }
  else {
    _sigma_s2_cpu = (_mean_s2_cpu - _mean_s_cpu) * (_mean_s2_cpu - _mean_s_cpu) / 4;
    //do not allow too small sigma
    if (_sigma_s2_cpu < _step * _step / 6.28)
      _sigma_s2_cpu = _step * _step / 6.28;

    if (_debug) {
      if (sum2 <= 0)
        cout << "All slices are equal. ";
      if (den2 <= 0)
        cout << "All slices inliers. ";
      cout << "Setting sigma_s2 to " << sqrt(_sigma_s2_cpu) << endl;
    }
  }

  //Calculate slice weights
  double gs1, gs2;
  for (inputIndex = 0; inputIndex < _slices.size(); inputIndex++) {
    //Slice does not have any voxels in volumetric ROI
    if (slice_potential_cpu[inputIndex] == -1) {
      _slice_weight_cpu[inputIndex] = 0;
      continue;
    }

    //All slices are outliers or the means are not valid
    if ((den <= 0) || (_mean_s2_cpu <= _mean_s_cpu)) {
      _slice_weight_cpu[inputIndex] = 1;
      continue;
    }

    //likelihood for inliers
    if (slice_potential_cpu[inputIndex] < _mean_s2_cpu)
      gs1 = G(slice_potential_cpu[inputIndex] - _mean_s_cpu, _sigma_s_cpu);
    else
      gs1 = 0;

    //likelihood for outliers
    if (slice_potential_cpu[inputIndex] > _mean_s_cpu)
      gs2 = G(slice_potential_cpu[inputIndex] - _mean_s2_cpu, _sigma_s2_cpu);
    else
      gs2 = 0;

    //calculate slice weight
    double likelihood = gs1 * _mix_s_cpu + gs2 * (1 - _mix_s_cpu);
    if (likelihood > 0)
      _slice_weight_cpu[inputIndex] = gs1 * _mix_s_cpu / likelihood;
    else {
      if (slice_potential_cpu[inputIndex] <= _mean_s_cpu)
        _slice_weight_cpu[inputIndex] = 1;
      if (slice_potential_cpu[inputIndex] >= _mean_s2_cpu)
        _slice_weight_cpu[inputIndex] = 0;
      if ((slice_potential_cpu[inputIndex] < _mean_s2_cpu) && (slice_potential_cpu[inputIndex] > _mean_s_cpu)) //should not happen
        _slice_weight_cpu[inputIndex] = 1;
    }
  }

  //Update _mix_s this should also be part of MStep
  sum = 0;
  num = 0;
  for (inputIndex = 0; inputIndex < _slices.size(); inputIndex++)
    if (slice_potential_cpu[inputIndex] >= 0) {
    sum += _slice_weight_cpu[inputIndex];
    num++;
    }

  if (num > 0)
    _mix_s_cpu = sum / num;
  else {
    cout << "All slices are outliers. Setting _mix_s to 0.9." << endl;
    _mix_s_cpu = 0.9;
  }

  if (_debug || _debugGPU) {
    cout << setprecision(3);
    cout << "Slice robust statistics parameters CPU: ";
    cout << "means: " << _mean_s_cpu << " " << _mean_s2_cpu << "  ";
    cout << "sigmas: " << sqrt(_sigma_s_cpu) << " " << sqrt(_sigma_s2_cpu) << "  ";
    cout << "proportions: " << _mix_s_cpu << " " << 1 - _mix_s_cpu << endl;
    cout << "Slice weights  CPU: ";
    for (inputIndex = 0; inputIndex < _slices.size(); inputIndex++)
      cout << _slice_weight_cpu[inputIndex] << " ";
    /*cout << "Slice potential: ";
    for (inputIndex = 0; inputIndex < _slices.size(); inputIndex++)
    cout << slice_potential[inputIndex] << " ";*/
    cout << endl;
  }

}

class ParallelScale {
  irtkReconstruction *reconstructor;

public:
  ParallelScale(irtkReconstruction *_reconstructor) :
    reconstructor(_reconstructor)
  { }

  void operator() (const blocked_range<size_t> &r) const {
    for (size_t inputIndex = r.begin(); inputIndex != r.end(); ++inputIndex) {

      // alias the current slice
      irtkRealImage& slice = reconstructor->_slices[inputIndex];

      //alias the current weight image
      irtkRealImage& w = reconstructor->_weights[inputIndex];

      //alias the current bias image
      irtkRealImage& b = reconstructor->_bias[inputIndex];

      //initialise calculation of scale
      double scalenum = 0;
      double scaleden = 0;

      for (int i = 0; i < slice.GetX(); i++)
        for (int j = 0; j < slice.GetY(); j++)
          if (slice(i, j, 0) != -1) {
        if (reconstructor->_simulated_weights[inputIndex](i, j, 0) > 0.99) {
          //scale - intensity matching
          double eb = exp(-b(i, j, 0));
          scalenum += w(i, j, 0) * slice(i, j, 0) * eb * reconstructor->_simulated_slices[inputIndex](i, j, 0);
          scaleden += w(i, j, 0) * slice(i, j, 0) * eb * slice(i, j, 0) * eb;
        }
          }

      //calculate scale for this slice
      if (scaleden > 0)
        reconstructor->_scale_cpu[inputIndex] = scalenum / scaleden;
      else
        reconstructor->_scale_cpu[inputIndex] = 1;

    } //end of loop for a slice inputIndex  
  }

  // execute
  void operator() () const {
    task_scheduler_init init(tbb_no_threads);
    parallel_for(blocked_range<size_t>(0, reconstructor->_slices.size()),
      *this);
    init.terminate();
  }

};

void irtkReconstruction::ScaleGPU()
{
  if (_debug)
    cout << "Scale" << endl;

  reconstructionGPU->CalculateScaleVector(_scale_gpu);
  //_scale_gpu = reconstructionGPU->h_scales;
  if (_debug || _debugGPU) {
    cout << setprecision(3);
    cout << "Slice scale GPU= ";
    for (unsigned int inputIndex = 0; inputIndex < _slices.size(); ++inputIndex)
      cout << inputIndex << ":" << _scale_gpu[inputIndex] << " ";
    cout << endl;
  }
}

void irtkReconstruction::Scale()
{
  if (_debug)
    cout << "Scale" << endl;

  ParallelScale parallelScale(this);
  parallelScale();

  //Normalise scales by setting geometric mean to 1
  // now abandoned
  //if (!_global_bias_correction)
  //{
  //  double product = 1;
  //  for (inputIndex = 0; inputIndex < _slices.size(); inputIndex++)
  //      product *= _scale[inputIndex];
  //  product = pow(product, 1.0 / _slices.size());
  //  for (inputIndex = 0; inputIndex < _slices.size(); inputIndex++)
  //      _scale[inputIndex] /= product;
  //}

  if (_debug || _debugGPU) {
    cout << setprecision(3);
    cout << "Slice scale CPU= ";
    for (unsigned int inputIndex = 0; inputIndex < _slices.size(); ++inputIndex)
      cout << inputIndex << ":" << _scale_cpu[inputIndex] << " ";
    cout << endl;
  }
}

class ParallelBias {
  irtkReconstruction* reconstructor;

public:

  void operator()(const blocked_range<size_t>& r) const {
    for (size_t inputIndex = r.begin(); inputIndex < r.end(); ++inputIndex) {
      // read the current slice
      irtkRealImage slice = reconstructor->_slices[inputIndex];

      //alias the current weight image
      irtkRealImage& w = reconstructor->_weights[inputIndex];

      //alias the current bias image
      irtkRealImage b = reconstructor->_bias[inputIndex];

      //identify scale factor
      double scale = reconstructor->_scale_cpu[inputIndex];

      //prepare weight image for bias field
      irtkRealImage wb = w;

      //simulated slice
      // irtkRealImage sim;
      // sim.Initialize( slice.GetImageAttributes() );
      // sim = 0;
      irtkRealImage wresidual(slice.GetImageAttributes());
      wresidual = 0;

      for (int i = 0; i < slice.GetX(); i++)
        for (int j = 0; j < slice.GetY(); j++)
          if (slice(i, j, 0) != -1) {
        if (reconstructor->_simulated_weights[inputIndex](i, j, 0) > 0.99) {
          //bias-correct and scale current slice
          double eb = exp(-b(i, j, 0));
          slice(i, j, 0) *= (eb * scale);

          //calculate weight image
          wb(i, j, 0) = w(i, j, 0) * slice(i, j, 0);

          //calculate weighted residual image
          //make sure it is far from zero to avoid numerical instability
          //if ((sim(i,j,0)>_low_intensity_cutoff*_max_intensity)&&(slice(i,j,0)>_low_intensity_cutoff*_max_intensity))
          if ((reconstructor->_simulated_slices[inputIndex](i, j, 0) > 1) && (slice(i, j, 0) > 1)) {
            wresidual(i, j, 0) = log(slice(i, j, 0) / reconstructor->_simulated_slices[inputIndex](i, j, 0)) * wb(i, j, 0);
          }
        }
        else {
          //do not take into account this voxel when calculating bias field
          wresidual(i, j, 0) = 0;
          wb(i, j, 0) = 0;
        }
          }

      //calculate bias field for this slice
      irtkGaussianBlurring<irtkRealPixel> gb(reconstructor->_sigma_bias);
      //smooth weighted residual
      gb.SetInput(&wresidual);
      gb.SetOutput(&wresidual);
      gb.Run();

      //smooth weight image
      gb.SetInput(&wb);
      gb.SetOutput(&wb);
      gb.Run();

      //update bias field
      double sum = 0;
      double num = 0;
      for (int i = 0; i < slice.GetX(); i++)
        for (int j = 0; j < slice.GetY(); j++)
          if (slice(i, j, 0) != -1) {
        if (wb(i, j, 0) > 0)
          b(i, j, 0) += wresidual(i, j, 0) / wb(i, j, 0);
        sum += b(i, j, 0);
        num++;
          }

      //normalize bias field to have zero mean
      if (!reconstructor->_global_bias_correction) {
        double mean = 0;
        if (num > 0)
          mean = sum / num;
        for (int i = 0; i < slice.GetX(); i++)
          for (int j = 0; j < slice.GetY(); j++)
            if ((slice(i, j, 0) != -1) && (num > 0)) {
          b(i, j, 0) -= mean;
            }
      }

      reconstructor->_bias[inputIndex] = b;
    }
  }

  ParallelBias(irtkReconstruction *reconstructor) :
    reconstructor(reconstructor)
  { }

  // execute
  void operator() () const {
    task_scheduler_init init(tbb_no_threads);
    parallel_for(blocked_range<size_t>(0, reconstructor->_slices.size()),
      *this);
    init.terminate();
  }

};

void irtkReconstruction::BiasGPU()
{

  if (_debug)
    cout << "Correcting bias ...";

  if (_global_bias_correction)
    printf("_global_bias_correction is not yet fully implemented in CUDA\n");

  reconstructionGPU->CorrectBias(_sigma_bias, _global_bias_correction); //assuming globally constant pixel size
  if(_debugGPU)
  {
    irtkGenericImage<float> bimg(reconstructionGPU->v_bias.size.x, reconstructionGPU->v_bias.size.y, reconstructionGPU->v_bias.size.z);
    reconstructionGPU->debugBias(bimg.GetPointerToVoxels());
    bimg.Write("biasFieldGPU.nii");
  }

  if (_debug)
    cout << "done. " << endl;
}


void irtkReconstruction::Bias()
{
  if (_debug)
    cout << "Correcting bias ...";

  ParallelBias parallelBias(this);
  parallelBias();

  _bias[79].Write("biasField79CPU.nii");

  if (_debug)
    cout << "done. " << endl;
}

class ParallelSuperresolution {
  irtkReconstruction* reconstructor;
public:
  irtkRealImage confidence_map;
  irtkRealImage addon;

  void operator()(const blocked_range<size_t>& r) {
    for (size_t inputIndex = r.begin(); inputIndex < r.end(); ++inputIndex) {
      // read the current slice
      irtkRealImage slice = reconstructor->_slices[inputIndex];

      //read the current weight image
      irtkRealImage& w = reconstructor->_weights[inputIndex];

      //read the current bias image
      irtkRealImage& b = reconstructor->_bias[inputIndex];

      //identify scale factor
      double scale = reconstructor->_scale_cpu[inputIndex];

      //Update reconstructed volume using current slice

      //Distribute error to the volume
      POINT3D p;
      for (int i = 0; i < slice.GetX(); i++)
        for (int j = 0; j < slice.GetY(); j++)
          if (slice(i, j, 0) != -1) {
        //bias correct and scale the slice
        slice(i, j, 0) *= exp(-b(i, j, 0)) * scale;

        if (reconstructor->_simulated_slices[inputIndex](i, j, 0) > 0)
          slice(i, j, 0) -= reconstructor->_simulated_slices[inputIndex](i, j, 0);
        else
          slice(i, j, 0) = 0;

        int n = reconstructor->_volcoeffs[inputIndex][i][j].size();
        for (int k = 0; k < n; k++) {
          p = reconstructor->_volcoeffs[inputIndex][i][j][k];
          addon(p.x, p.y, p.z) += p.value * slice(i, j, 0) * w(i, j, 0) * reconstructor->_slice_weight_cpu[inputIndex];
          confidence_map(p.x, p.y, p.z) += p.value * w(i, j, 0) * reconstructor->_slice_weight_cpu[inputIndex];
        }
          }
    } //end of loop for a slice inputIndex
  }

  ParallelSuperresolution(ParallelSuperresolution& x, split) :
    reconstructor(x.reconstructor)
  {
    //Clear addon
    addon.Initialize(reconstructor->_reconstructed.GetImageAttributes());
    addon = 0;

    //Clear confidence map
    confidence_map.Initialize(reconstructor->_reconstructed.GetImageAttributes());
    confidence_map = 0;
  }

  void join(const ParallelSuperresolution& y) {
    addon += y.addon;
    confidence_map += y.confidence_map;
  }

  ParallelSuperresolution(irtkReconstruction *reconstructor) :
    reconstructor(reconstructor)
  {
    //Clear addon
    addon.Initialize(reconstructor->_reconstructed.GetImageAttributes());
    addon = 0;

    //Clear confidence map
    confidence_map.Initialize(reconstructor->_reconstructed.GetImageAttributes());
    confidence_map = 0;
  }

  // execute
  void operator() () {
    task_scheduler_init init(tbb_no_threads);
    parallel_reduce(blocked_range<size_t>(0, reconstructor->_slices.size()),
      *this);
    init.terminate();
  }
};

void irtkReconstruction::SuperresolutionGPU(int iter)
{
  if (_debug)
    cout << "Superresolution " << iter << endl;

  int i, j, k;
  irtkRealImage addon, original;
  //Remember current reconstruction for edge-preserving smoothing
  original = _reconstructed;

  reconstructionGPU->Superresolution(iter, _slice_weight_gpu, _adaptive, _alpha, _min_intensity, _max_intensity, _delta,
    _lambda, _global_bias_correction, _sigma_bias, _low_intensity_cutoff); //assuming isotrop constant voxel size

  //TODO debug confidence map and addon
  if(_debugGPU)
  {
    char buffer[256];
    irtkGenericImage<float> addonDB;
    addonDB.Initialize(_reconstructed.GetImageAttributes());
    reconstructionGPU->debugAddon(addonDB.GetPointerToVoxels());
    sprintf(buffer, "addonGPU%i.nii", iter - 1);
    addonDB.Write(buffer);
    irtkGenericImage<float> cmap;
    cmap.Initialize(_reconstructed.GetImageAttributes());
    reconstructionGPU->debugConfidenceMap(cmap.GetPointerToVoxels());
    sprintf(buffer, "cmapGPU%i.nii", iter - 1);
    cmap.Write(buffer);
}

}

void irtkReconstruction::Superresolution(int iter)
{
  if (_debug)
    cout << "Superresolution " << iter << endl;

  int i, j, k;
  irtkRealImage addon, original;

  //Remember current reconstruction for edge-preserving smoothing
  original = _reconstructed;

  ParallelSuperresolution parallelSuperresolution(this);
  parallelSuperresolution();
  addon = parallelSuperresolution.addon;
  _confidence_map = parallelSuperresolution.confidence_map;
  //_confidence4mask = _confidence_map;

  if (_debug) {
    char buffer[256];
    sprintf(buffer, "confidence-map%i.nii.gz", iter);
    _confidence_map.Write(buffer);
    sprintf(buffer, "addon%i.nii.gz", iter);
    addon.Write(buffer);
  }

  if (!_adaptive)
    for (i = 0; i < addon.GetX(); i++)
      for (j = 0; j < addon.GetY(); j++)
        for (k = 0; k < addon.GetZ(); k++)
          if (_confidence_map(i, j, k) > 0) {
    // ISSUES if _confidence_map(i, j, k) is too small leading
    // to bright pixels
    addon(i, j, k) /= _confidence_map(i, j, k);
    //this is to revert to normal (non-adaptive) regularisation
    _confidence_map(i, j, k) = 1;
          }

  _reconstructed += addon * _alpha; //_average_volume_weight;

  //bound the intensities
  for (i = 0; i < _reconstructed.GetX(); i++)
    for (j = 0; j < _reconstructed.GetY(); j++)
      for (k = 0; k < _reconstructed.GetZ(); k++) {
    if (_reconstructed(i, j, k) < _min_intensity * 0.9)
      _reconstructed(i, j, k) = _min_intensity * 0.9;
    if (_reconstructed(i, j, k) > _max_intensity * 1.1)
      _reconstructed(i, j, k) = _max_intensity * 1.1;
      }

  //Smooth the reconstructed image
  AdaptiveRegularization(iter, original);
  //Remove the bias in the reconstructed volume compared to previous iteration
  if (_global_bias_correction)
    BiasCorrectVolume(original);

  if(_debugGPU)
  {
    char buffer[256];
    sprintf(buffer, "addonCPU%i.nii", iter - 1);
    addon.Write(buffer);

    sprintf(buffer, "cmapCPU%i.nii", iter - 1);
    _confidence_map.Write(buffer);
}
}

class ParallelMStep{
  irtkReconstruction* reconstructor;
public:
  double sigma;
  double mix;
  double num;
  double min;
  double max;

  void operator()(const blocked_range<size_t>& r) {
    for (size_t inputIndex = r.begin(); inputIndex < r.end(); ++inputIndex) {
      // read the current slice
      irtkRealImage slice = reconstructor->_slices[inputIndex];

      //alias the current weight image
      irtkRealImage& w = reconstructor->_weights[inputIndex];

      //alias the current bias image
      irtkRealImage& b = reconstructor->_bias[inputIndex];

      //identify scale factor
      double scale = reconstructor->_scale_cpu[inputIndex];

      //calculate error
      for (int i = 0; i < slice.GetX(); i++)
        for (int j = 0; j < slice.GetY(); j++)
          if (slice(i, j, 0) != -1) {
        //bias correct and scale the slice
        slice(i, j, 0) *= exp(-b(i, j, 0)) * scale;

        //otherwise the error has no meaning - it is equal to slice intensity
        if (reconstructor->_simulated_weights[inputIndex](i, j, 0) > 0.99) {

          slice(i, j, 0) -= reconstructor->_simulated_slices[inputIndex](i, j, 0);

          //sigma and mix
          double e = slice(i, j, 0);
          sigma += e * e * w(i, j, 0);
          mix += w(i, j, 0);

          //_m
          if (e < min)
            min = e;
          if (e > max)
            max = e;

          num++;
        }
          }
    } //end of loop for a slice inputIndex
  }

  ParallelMStep(ParallelMStep& x, split) :
    reconstructor(x.reconstructor)
  {
    sigma = 0;
    mix = 0;
    num = 0;
    min = 0;
    max = 0;
  }

  void join(const ParallelMStep& y) {
    if (y.min < min)
      min = y.min;
    if (y.max > max)
      max = y.max;

    sigma += y.sigma;
    mix += y.mix;
    num += y.num;
  }

  ParallelMStep(irtkReconstruction *reconstructor) :
    reconstructor(reconstructor)
  {
    sigma = 0;
    mix = 0;
    num = 0;
    min = voxel_limits<irtkRealPixel>::max();
    max = voxel_limits<irtkRealPixel>::min();
  }

  // execute
  void operator() () {
    task_scheduler_init init(tbb_no_threads);
    parallel_reduce(blocked_range<size_t>(0, reconstructor->_slices.size()),
      *this);
    init.terminate();
  }
};


void irtkReconstruction::MStepGPU(int iter)
{
  reconstructionGPU->MStep(iter, _step, _sigma_gpu, _mix_gpu, _m_gpu);
  std::cout.precision(10);
  if (_debug || _debugGPU) {
    cout << "Voxel-wise robust statistics parameters GPU: ";
    cout << "sigma = " << sqrt(_sigma_gpu) << " mix = " << _mix_gpu << " ";
    cout << " m = " << _m_gpu << endl;
  }

}

void irtkReconstruction::MStep(int iter)
{
  if (_debug)
    cout << "MStep" << endl;

  ParallelMStep parallelMStep(this);
  parallelMStep();
  double sigma = parallelMStep.sigma;
  double mix = parallelMStep.mix;
  double num = parallelMStep.num;
  double min = parallelMStep.min;
  double max = parallelMStep.max;
  //printf("CPU sigma %f, mix %f, num %f, min_ %f, max_ %f\n", sigma, mix, num, min, max);
  std::cout.precision(6);
  std::cout << "CPU sigma " << sigma << " mix " << mix << " num " << num << " min_ " << min << " max_ " << max << std::endl;
  //Calculate sigma and mix
  if (mix > 0) {
    _sigma_cpu = sigma / mix;
  }
  else {
    cerr << "Something went wrong: sigma=" << sigma << " mix=" << mix << endl;
    exit(1);
  }
  if (_sigma_cpu < _step * _step / 6.28)
    _sigma_cpu = _step * _step / 6.28;
  if (iter > 1)
    _mix_cpu = mix / num;

  //Calculate m
  _m_cpu = 1 / (max - min);

  if (_debug || _debugGPU) {
    cout << "Voxel-wise robust statistics parameters CPU: ";
    cout << "sigma = " << sqrt(_sigma_cpu) << " mix = " << _mix_cpu << " ";
    cout << " m = " << _m_cpu << endl;
  }

}

class ParallelAdaptiveRegularization1 {
  irtkReconstruction *reconstructor;
  vector<irtkRealImage> &b;
  vector<double> &factor;
  irtkRealImage &original;

public:
  ParallelAdaptiveRegularization1(irtkReconstruction *_reconstructor,
    vector<irtkRealImage> &_b,
    vector<double> &_factor,
    irtkRealImage &_original) :
    reconstructor(_reconstructor),
    b(_b),
    factor(_factor),
    original(_original) { }

  void operator() (const blocked_range<size_t> &r) const {
    int dx = reconstructor->_reconstructed.GetX();
    int dy = reconstructor->_reconstructed.GetY();
    int dz = reconstructor->_reconstructed.GetZ();
    for (size_t i = r.begin(); i != r.end(); ++i) {
      //b[i] = reconstructor->_reconstructed;
      // b[i].Initialize( reconstructor->_reconstructed.GetImageAttributes() );

      int x, y, z, xx, yy, zz;
      double diff;
      for (x = 0; x < dx; x++)
        for (y = 0; y < dy; y++)
          for (z = 0; z < dz; z++) {
        xx = x + reconstructor->_directions[i][0];
        yy = y + reconstructor->_directions[i][1];
        zz = z + reconstructor->_directions[i][2];
        if ((xx >= 0) && (xx < dx) && (yy >= 0) && (yy < dy) && (zz >= 0) && (zz < dz)
          && (reconstructor->_confidence_map(x, y, z) > 0) && (reconstructor->_confidence_map(xx, yy, zz) > 0)) {
          diff = (original(xx, yy, zz) - original(x, y, z)) * sqrt(factor[i]) / reconstructor->_delta;
          b[i](x, y, z) = factor[i] / sqrt(1 + diff * diff);

        }
        else
          b[i](x, y, z) = 0;
          }
    }
  }

  // execute
  void operator() () const {
    task_scheduler_init init(tbb_no_threads);
    parallel_for(blocked_range<size_t>(0, 13),
      *this);
    init.terminate();
  }

};

class ParallelAdaptiveRegularization2 {
  irtkReconstruction *reconstructor;
  vector<irtkRealImage> &b;
  vector<double> &factor;
  irtkRealImage &original;

public:
  ParallelAdaptiveRegularization2(irtkReconstruction *_reconstructor,
    vector<irtkRealImage> &_b,
    vector<double> &_factor,
    irtkRealImage &_original) :
    reconstructor(_reconstructor),
    b(_b),
    factor(_factor),
    original(_original) { }

  void operator() (const blocked_range<size_t> &r) const {
    int dx = reconstructor->_reconstructed.GetX();
    int dy = reconstructor->_reconstructed.GetY();
    int dz = reconstructor->_reconstructed.GetZ();
    for (size_t x = r.begin(); x != r.end(); ++x) {
      int xx, yy, zz;
      for (int y = 0; y < dy; y++)
        for (int z = 0; z < dz; z++) {
        double val = 0;
        double valW = 0;
        double sum = 0;
        for (int i = 0; i < 13; i++) {
          xx = x + reconstructor->_directions[i][0];
          yy = y + reconstructor->_directions[i][1];
          zz = z + reconstructor->_directions[i][2];
          if ((xx >= 0) && (xx < dx) && (yy >= 0) && (yy < dy) && (zz >= 0) && (zz < dz)) {
            val += b[i](x, y, z) * original(xx, yy, zz) * reconstructor->_confidence_map(xx, yy, zz);
            valW += b[i](x, y, z) * reconstructor->_confidence_map(xx, yy, zz);
            sum += b[i](x, y, z);
          }
        }

        for (int i = 0; i < 13; i++) {
          xx = x - reconstructor->_directions[i][0];
          yy = y - reconstructor->_directions[i][1];
          zz = z - reconstructor->_directions[i][2];
          if ((xx >= 0) && (xx < dx) && (yy >= 0) && (yy < dy) && (zz >= 0) && (zz < dz)) {
            val += b[i](xx, yy, zz) * original(xx, yy, zz) * reconstructor->_confidence_map(xx, yy, zz);
            valW += b[i](xx, yy, zz) * reconstructor->_confidence_map(xx, yy, zz);
            sum += b[i](xx, yy, zz);
          }
        }

        val -= sum * original(x, y, z) * reconstructor->_confidence_map(x, y, z);
        valW -= sum * reconstructor->_confidence_map(x, y, z);
        val = original(x, y, z) * reconstructor->_confidence_map(x, y, z)
          + reconstructor->_alpha * reconstructor->_lambda / (reconstructor->_delta * reconstructor->_delta) * val;
        valW = reconstructor->_confidence_map(x, y, z) + reconstructor->_alpha * reconstructor->_lambda / (reconstructor->_delta * reconstructor->_delta) * valW;

        if (valW > 0) {
          reconstructor->_reconstructed(x, y, z) = val / valW;
        }
        else
          reconstructor->_reconstructed(x, y, z) = 0;
        }

    }
  }

  // execute
  void operator() () const {
    task_scheduler_init init(tbb_no_threads);
    parallel_for(blocked_range<size_t>(0, reconstructor->_reconstructed.GetX()),
      *this);
    init.terminate();
  }

};

void irtkReconstruction::AdaptiveRegularization(int iter, irtkRealImage& original)
{
  if (_debug)
    cout << "AdaptiveRegularization" << endl;

  vector<double> factor(13, 0);
  for (int i = 0; i < 13; i++) {
    for (int j = 0; j < 3; j++)
      factor[i] += fabs(double(_directions[i][j]));
    factor[i] = 1 / factor[i];
  }

  vector<irtkRealImage> b;//(13);
  for (int i = 0; i < 13; i++)
    b.push_back(_reconstructed);

  ParallelAdaptiveRegularization1 parallelAdaptiveRegularization1(this,
    b,
    factor,
    original);
  parallelAdaptiveRegularization1();

  irtkRealImage original2 = _reconstructed;
  ParallelAdaptiveRegularization2 parallelAdaptiveRegularization2(this,
    b,
    factor,
    original2);
  parallelAdaptiveRegularization2();

  if (_alpha * _lambda / (_delta * _delta) > 0.068) {
    cerr
      << "Warning: regularization might not have smoothing effect! Ensure that alpha*lambda/delta^2 is below 0.068."
      << endl;
  }
}

void irtkReconstruction::BiasCorrectVolume(irtkRealImage& original)
{
  //remove low-frequancy component in the reconstructed image which might have accured due to overfitting of the biasfield
  irtkRealImage residual = _reconstructed;
  irtkRealImage weights = _mask;

  //_reconstructed.Write("super-notbiascor.nii.gz");

  //calculate weighted residual
  irtkRealPixel *pr = residual.GetPointerToVoxels();
  irtkRealPixel *po = original.GetPointerToVoxels();
  irtkRealPixel *pw = weights.GetPointerToVoxels();
  for (int i = 0; i < _reconstructed.GetNumberOfVoxels(); i++) {
    //second and term to avoid numerical problems
    if ((*pw == 1) && (*po > _low_intensity_cutoff * _max_intensity)
      && (*pr > _low_intensity_cutoff * _max_intensity)) {
      *pr /= *po;
      *pr = log(*pr);
    }
    else {
      *pw = 0;
      *pr = 0;
    }
    pr++;
    po++;
    pw++;
  }
  //residual.Write("residual.nii.gz");
  //blurring needs to be same as for slices
  irtkGaussianBlurring<irtkRealPixel> gb(_sigma_bias);
  //blur weigted residual
  gb.SetInput(&residual);
  gb.SetOutput(&residual);
  gb.Run();
  //blur weight image
  gb.SetInput(&weights);
  gb.SetOutput(&weights);
  gb.Run();

  //calculate the bias field
  pr = residual.GetPointerToVoxels();
  pw = weights.GetPointerToVoxels();
  irtkRealPixel *pm = _mask.GetPointerToVoxels();
  irtkRealPixel *pi = _reconstructed.GetPointerToVoxels();
  for (int i = 0; i < _reconstructed.GetNumberOfVoxels(); i++) {

    if (*pm == 1) {
      //weighted gaussian smoothing
      *pr /= *pw;
      //exponential to recover multiplicative bias field
      *pr = exp(*pr);
      //bias correct reconstructed
      *pi /= *pr;
      //clamp intensities to allowed range
      if (*pi < _min_intensity * 0.9)
        *pi = _min_intensity * 0.9;
      if (*pi > _max_intensity * 1.1)
        *pi = _max_intensity * 1.1;
    }
    else {
      *pr = 0;
    }
    pr++;
    pw++;
    pm++;
    pi++;
  }

  //residual.Write("biasfield.nii.gz");
  //_reconstructed.Write("super-biascor.nii.gz");

}

void irtkReconstruction::EvaluateGPU(int iter)
{
  cout << "Iteration " << iter << ": " << endl;

  cout << "Included slices GPU: ";
  int sum = 0;
  unsigned int i;
  for (i = 0; i < _slices.size(); i++) {
    if ((_slice_weight_gpu[i] >= 0.5) && (_slice_inside_gpu[i])) {
      cout << i << " ";
      sum++;
    }
  }
  cout << endl << "Total GPU: " << sum << endl;

  cout << "Excluded slices  GPU: ";
  sum = 0;
  for (i = 0; i < _slices.size(); i++) {
    if ((_slice_weight_gpu[i] < 0.5) && (_slice_inside_gpu[i])) {
      cout << i << " ";
      sum++;
    }
  }
  cout << endl << "Total GPU: " << sum << endl;

  cout << "Outside slices GPU: ";
  sum = 0;
  for (i = 0; i < _slices.size(); i++) {
    if (!(_slice_inside_gpu[i])) {
      cout << i << " ";
      sum++;
    }
  }
  cout << endl << "Total GPU: " << sum << endl;

}

void irtkReconstruction::Evaluate(int iter)
{
  cout << "Iteration " << iter << ": " << endl;

  cout << "Included slices CPU: ";
  int sum = 0;
  unsigned int i;
  for (i = 0; i < _slices.size(); i++) {
    if ((_slice_weight_cpu[i] >= 0.5) && (_slice_inside_cpu[i])) {
      cout << i << " ";
      sum++;
    }
  }
  cout << endl << "Total: " << sum << endl;

  cout << "Excluded slices CPU: ";
  sum = 0;
  for (i = 0; i < _slices.size(); i++) {
    if ((_slice_weight_cpu[i] < 0.5) && (_slice_inside_cpu[i])) {
      cout << i << " ";
      sum++;
    }
  }
  cout << endl << "Total CPU: " << sum << endl;

  cout << "Outside slices CPU: ";
  sum = 0;
  for (i = 0; i < _slices.size(); i++) {
    if (!(_slice_inside_cpu[i])) {
      cout << i << " ";
      sum++;
    }
  }
  cout << endl << "Total CPU: " << sum << endl;

}


class ParallelNormaliseBias{
  irtkReconstruction* reconstructor;
public:
  irtkRealImage bias;

  void operator()(const blocked_range<size_t>& r) {
    for (size_t inputIndex = r.begin(); inputIndex < r.end(); ++inputIndex) {

      if (reconstructor->_debug) {
        cout << inputIndex << " ";
      }

      // alias the current slice
      irtkRealImage& slice = reconstructor->_slices[inputIndex];

      //read the current bias image
      irtkRealImage b = reconstructor->_bias[inputIndex];

      //read current scale factor
      double scale = reconstructor->_scale_cpu[inputIndex];

      irtkRealPixel *pi = slice.GetPointerToVoxels();
      irtkRealPixel *pb = b.GetPointerToVoxels();
      for (int i = 0; i<slice.GetNumberOfVoxels(); i++) {
        if ((*pi>-1) && (scale > 0))
          *pb -= log(scale);
        pb++;
        pi++;
      }

      //Distribute slice intensities to the volume
      POINT3D p;
      for (int i = 0; i < slice.GetX(); i++)
        for (int j = 0; j < slice.GetY(); j++)
          if (slice(i, j, 0) != -1) {
        //number of volume voxels with non-zero coefficients for current slice voxel
        int n = reconstructor->_volcoeffs[inputIndex][i][j].size();
        //add contribution of current slice voxel to all voxel volumes
        //to which it contributes
        for (int k = 0; k < n; k++) {
          p = reconstructor->_volcoeffs[inputIndex][i][j][k];
          bias(p.x, p.y, p.z) += p.value * b(i, j, 0);
        }
          }
      //end of loop for a slice inputIndex                
    }
  }

  ParallelNormaliseBias(ParallelNormaliseBias& x, split) :
    reconstructor(x.reconstructor)
  {
    bias.Initialize(reconstructor->_reconstructed.GetImageAttributes());
    bias = 0;
  }

  void join(const ParallelNormaliseBias& y) {
    bias += y.bias;
  }

  ParallelNormaliseBias(irtkReconstruction *reconstructor) :
    reconstructor(reconstructor)
  {
    bias.Initialize(reconstructor->_reconstructed.GetImageAttributes());
    bias = 0;
  }

  // execute
  void operator() () {
    task_scheduler_init init(tbb_no_threads);
    parallel_reduce(blocked_range<size_t>(0, reconstructor->_slices.size()),
      *this);
    init.terminate();
  }
};

void irtkReconstruction::NormaliseBiasGPU(int iter)
{
  reconstructionGPU->NormaliseBias(iter, _sigma_bias);

  if(_debugGPU)
  {
    char buffer[256];
    irtkGenericImage<float> nbias;
    nbias.Initialize(_reconstructed.GetImageAttributes());
    reconstructionGPU->debugNormalizeBias(nbias.GetPointerToVoxels());
    sprintf(buffer, "nbiasGPU%i.nii", iter);
    nbias.Write(buffer);

    irtkGenericImage<float> smask;

    smask.Initialize(_mask.GetImageAttributes());
    reconstructionGPU->debugSmoothMask(smask.GetPointerToVoxels());
    sprintf(buffer, "smaskGPU%i.nii", iter);
    smask.Write(buffer);
}
}

void irtkReconstruction::NormaliseBias(int iter)
{
  if (_debug)
    cout << "Normalise Bias ... ";

  ParallelNormaliseBias parallelNormaliseBias(this);
  parallelNormaliseBias();
  irtkRealImage bias = parallelNormaliseBias.bias;

  // normalize the volume by proportion of contributing slice voxels for each volume voxel
  bias /= _volume_weights;

  if (_debug)
    cout << "done." << endl;

  MaskImage(bias, 0);
  irtkRealImage m = _mask;
  irtkGaussianBlurring<irtkRealPixel> gb(_sigma_bias);
  gb.SetInput(&bias);
  gb.SetOutput(&bias);
  gb.Run();
  gb.SetInput(&m);
  gb.SetOutput(&m);
  gb.Run();
  bias /= m;

  if (_debugGPU)
  {
    char buffer_[256];
    sprintf(buffer_, "smaskCPU%i.nii", iter);
    m.Write(buffer_);
  }

  if (_debug) {
    char buffer[256];
    sprintf(buffer, "averagebias%i.nii.gz", iter);
    bias.Write(buffer);
  }

  irtkRealPixel *pi, *pb;
  pi = _reconstructed.GetPointerToVoxels();
  pb = bias.GetPointerToVoxels();
  for (int i = 0; i < _reconstructed.GetNumberOfVoxels(); i++) {
    if (*pi != -1)
      *pi /= exp(-(*pb));
    pi++;
    pb++;
  }
  if (_debugGPU)
  {
    char buffer[256];
    sprintf(buffer, "nbiasCPU%i.nii", iter);
    bias.Write(buffer);
  }
}

/* Set/Get/Save operations */

void irtkReconstruction::ReadTransformation(char* folder)
{
  int n = _slices.size();
  char name[256];
  char path[256];
  irtkTransformation *transformation;
  irtkRigidTransformation *rigidTransf;

  if (n == 0) {
    cerr << "Please create slices before reading transformations!" << endl;
    exit(1);
  }
  cout << "Reading transformations:" << endl;

  _transformations.clear();
  _transformations_gpu.clear();
  for (int i = 0; i < n; i++) {
    if (folder != NULL) {
      sprintf(name, "/transformation%i.dof", i);
      strcpy(path, folder);
      strcat(path, name);
    }
    else {
      sprintf(path, "transformation%i.dof", i);
    }
    transformation = irtkTransformation::New(path);
    rigidTransf = dynamic_cast<irtkRigidTransformation*>(transformation);
    _transformations.push_back(*rigidTransf);
    _transformations_gpu.push_back(*rigidTransf);
    delete transformation;
    cout << path << endl;
  }
}

void irtkReconstruction::replaceSlices(string folder)
{
  //replaces slices with already transformed slices
  //renders every stack operation before useless -- TODO 

  //stack Id and transformations are preserved
  //The slices have to be in the correct order in the folder!

  //get file list from boost
  path p(folder);
  if (exists(p))
  {
    vector<path> files;
    copy(directory_iterator(p), directory_iterator(), back_inserter(files));

    vector<irtkRealImage> newSlices;
    vector<double> thickness;
    vector<int> stackId;
    vector<irtkRigidTransformation> transforms;

    //TODO ITK IRTK orientation mismatch?
    std::cout << "replacing every slice!!! " << std::endl;
    for (int i = 0; i < files.size()/*files.size()*/; i++)
    {
      //std::cout << files[i] << std::endl;
      irtkRealImage newSlice;
      newSlice.Read(files[i].string().c_str());
      //newSlice.Write(files[i].string().c_str());
      //newSlice.Read(files[i].string().c_str());
      //double axis[3];
      /*newSlice.GetOrientation(&axis[0], &axis[1], &axis[2]);
      axis[0] *= -1;
      axis[1] *= -1;
      axis[2] *= -1;
      newSlice.PutOrientation(&axis[0], &axis[1], &axis[2]);*/

      newSlices.push_back(newSlice);
      stackId.push_back(0); //TODO

      thickness.push_back(4.0);
      irtkTransformation *transformation = new irtkRigidTransformation;
      irtkRigidTransformation *rigidTransf = dynamic_cast<irtkRigidTransformation*> (transformation);
      // rigidTransf->Print();
      transforms.push_back(*rigidTransf);
      delete rigidTransf;
    }

    SetSlicesAndTransformations(newSlices, transforms, stackId, thickness);

  }
  else
  {
    cout << p << " does not exist\n";
  }

}



void irtkReconstruction::SetReconstructed(irtkRealImage &reconstructed)
{
  _reconstructed = reconstructed;
  _template_created = true;
}

void irtkReconstruction::SetTransformations(vector<irtkRigidTransformation>& transformations)
{
  _transformations.clear();
  _transformations_gpu.clear();
  for (int i = 0; i < transformations.size(); i++)
  {
    _transformations.push_back(transformations[i]);
    _transformations_gpu.push_back(transformations[i]);
  }

}

void irtkReconstruction::SaveBiasFields()
{
  char buffer[256];
  for (unsigned int inputIndex = 0; inputIndex < _slices.size(); inputIndex++) {
    sprintf(buffer, "bias%i.nii.gz", inputIndex);
    _bias[inputIndex].Write(buffer);
  }
}

void irtkReconstruction::SaveConfidenceMap()
{
  _confidence_map.Write("confidence-map.nii.gz");
}

void irtkReconstruction::SaveSlices()
{
  char buffer[256];
  for (unsigned int inputIndex = 0; inputIndex < _slices.size(); inputIndex++)
  {
    sprintf(buffer, "slice%i.nii.gz", inputIndex);
    _slices[inputIndex].Write(buffer);
  }
}

void irtkReconstruction::SaveWeights()
{
  char buffer[256];
  for (unsigned int inputIndex = 0; inputIndex < _slices.size(); inputIndex++) {
    sprintf(buffer, "weights%i.nii.gz", inputIndex);
    _weights[inputIndex].Write(buffer);
  }
}

void irtkReconstruction::SaveTransformations( const char* folder )
{
  char buffer[256];
  for (unsigned int inputIndex = 0; inputIndex < _slices.size(); inputIndex++) {
    sprintf(buffer, "%s/transformation%i.dof", folder, inputIndex);
    _transformations[inputIndex].irtkTransformation::Write(buffer);
    sprintf(buffer, "%s/transformation_gpu%i.dof", folder, inputIndex);
    _transformations_gpu[inputIndex].irtkTransformation::Write(buffer);
  }
}

void irtkReconstruction::GetTransformations(vector<irtkRigidTransformation> &transformations)
{
  transformations.clear();
  for (unsigned int inputIndex = 0; inputIndex < _slices.size(); inputIndex++)
  {
    transformations.push_back(_transformations[inputIndex]);
  }
}

void irtkReconstruction::GetSlices(vector<irtkRealImage> &slices)
{
  slices.clear();
  for (unsigned int i = 0; i < _slices.size(); i++)
    slices.push_back(_slices[i]);
}

void irtkReconstruction::SlicesInfo( const char* filename,
                                     vector<string> &stack_files )
{
    ofstream info;
    info.open( filename );

    // header
    info << "stack_index" << "\t"
         << "stack_name" << "\t"
         << "included" << "\t" // Included slices
         << "excluded" << "\t"  // Excluded slices
         << "outside" << "\t"  // Outside slices
         << "weight" << "\t"
         << "scale" << "\t"
         << "TranslationX" << "\t"
         << "TranslationY" << "\t"
         << "TranslationZ" << "\t"
         << "RotationX" << "\t"
         << "RotationY" << "\t"
         << "RotationZ" << endl;
    
    for (int i = 0; i < (int) _slices.size(); i++) {
        irtkRigidTransformation& t = _transformations_gpu[i];
        info << _stack_index[i] << "\t"
             << stack_files[_stack_index[i]] << "\t"
             << (((_slice_weight_gpu[i] >= 0.5) && (_slice_inside_gpu[i]))?1:0) << "\t" // Included slices
             << (((_slice_weight_gpu[i] < 0.5) && (_slice_inside_gpu[i]))?1:0) << "\t"  // Excluded slices
             << ((!(_slice_inside_gpu[i]))?1:0) << "\t"  // Outside slices
             << _slice_weight_gpu[i] << "\t"
             << _scale_gpu[i] << "\t"
             << t.GetTranslationX() << "\t"
             << t.GetTranslationY() << "\t"
             << t.GetTranslationZ() << "\t"
             << t.GetRotationX() << "\t"
             << t.GetRotationY() << "\t"
             << t.GetRotationZ() << endl;
    }
 
    info.close(); 
}

/* end Set/Get/Save operations */

/* Package specific functions */
void irtkReconstruction::SplitImage(irtkRealImage image, int packages, vector<irtkRealImage>& stacks)
{
  irtkImageAttributes attr = image.GetImageAttributes();

  //slices in package
  int pkg_z = attr._z / packages;
  double pkg_dz = attr._dz*packages;
  cout << "packages: " << packages << "; slices: " << attr._z << "; slices in package: " << pkg_z << endl;
  cout << "slice thickness " << attr._dz << "; slickess thickness in package: " << pkg_dz << endl;

  char buffer[256];
  int i, j, k, l;
  double x, y, z, sx, sy, sz, ox, oy, oz;
  for (l = 0; l < packages; l++) {
    attr = image.GetImageAttributes();
    if ((pkg_z*packages + l) < attr._z)
      attr._z = pkg_z + 1;
    else
      attr._z = pkg_z;
    attr._dz = pkg_dz;

    cout << "split image " << l << " has " << attr._z << " slices." << endl;

    //fill values in each stack
    irtkRealImage stack(attr);
    stack.GetOrigin(ox, oy, oz);

    cout << "Stack " << l << ":" << endl;
    for (k = 0; k < stack.GetZ(); k++)
      for (j = 0; j < stack.GetY(); j++)
        for (i = 0; i < stack.GetX(); i++)
          stack.Put(i, j, k, image(i, j, k*packages + l));

    //adjust origin

    //original image coordinates
    x = 0; y = 0; z = l;
    image.ImageToWorld(x, y, z);
    cout << "image: " << x << " " << y << " " << z << endl;
    //stack coordinates
    sx = 0; sy = 0; sz = 0;
    stack.PutOrigin(ox, oy, oz); //adjust to original value
    stack.ImageToWorld(sx, sy, sz);
    cout << "stack: " << sx << " " << sy << " " << sz << endl;
    //adjust origin
    cout << "adjustment needed: " << x - sx << " " << y - sy << " " << z - sz << endl;
    stack.PutOrigin(ox + (x - sx), oy + (y - sy), oz + (z - sz));
    sx = 0; sy = 0; sz = 0;
    stack.ImageToWorld(sx, sy, sz);
    cout << "adjusted: " << sx << " " << sy << " " << sz << endl;

    //sprintf(buffer,"stack%i.nii.gz",l);
    //stack.Write(buffer);
    stacks.push_back(stack);
  }
  cout << "done." << endl;

}

void irtkReconstruction::SplitImageEvenOdd(irtkRealImage image, int packages, vector<irtkRealImage>& stacks)
{
  vector<irtkRealImage> packs;
  vector<irtkRealImage> packs2;
  cout << "Split Image Even Odd: " << packages << " packages." << endl;

  stacks.clear();
  SplitImage(image, packages, packs);
  for (int i = 0; i < packs.size(); i++) {
    cout << "Package " << i << ": " << endl;
    packs2.clear();
    SplitImage(packs[i], 2, packs2);
    stacks.push_back(packs2[0]);
    stacks.push_back(packs2[1]);
  }

  cout << "done." << endl;
}

void irtkReconstruction::SplitImageEvenOddHalf(irtkRealImage image, int packages, vector<irtkRealImage>& stacks, int iter)
{
  vector<irtkRealImage> packs;
  vector<irtkRealImage> packs2;

  cout << "Split Image Even Odd Half " << iter << endl;
  stacks.clear();
  if (iter>1)
    SplitImageEvenOddHalf(image, packages, packs, iter - 1);
  else
    SplitImageEvenOdd(image, packages, packs);
  for (int i = 0; i < packs.size(); i++) {
    packs2.clear();
    HalfImage(packs[i], packs2);
    for (int j = 0; j < packs2.size(); j++)
      stacks.push_back(packs2[j]);
  }
}


void irtkReconstruction::HalfImage(irtkRealImage image, vector<irtkRealImage>& stacks)
{
  irtkRealImage tmp;
  irtkImageAttributes attr = image.GetImageAttributes();
  stacks.clear();

  //We would not like single slices - that is reserved for slice-to-volume
  if (attr._z >= 4) {
    tmp = image.GetRegion(0, 0, 0, attr._x, attr._y, attr._z / 2);
    stacks.push_back(tmp);
    tmp = image.GetRegion(0, 0, attr._z / 2, attr._x, attr._y, attr._z);
    stacks.push_back(tmp);
  }
  else
    stacks.push_back(image);
}


void irtkReconstruction::PackageToVolume(vector<irtkRealImage>& stacks, vector<int> &pack_num, bool evenodd, bool half, int half_iter)
{
  irtkImageRigidRegistrationWithPadding rigidregistration;
  irtkGreyImage t, s;
  //irtkRigidTransformation transformation;
  vector<irtkRealImage> packages;
  char buffer[256];

  int firstSlice = 0;
  cout << "Package to volume: " << endl;
  for (unsigned int i = 0; i < stacks.size(); i++) {
    cout << "Stack " << i << ": First slice index is " << firstSlice << endl;

    packages.clear();
    if (evenodd) {
      if (half)
        SplitImageEvenOddHalf(stacks[i], pack_num[i], packages, half_iter);
      else
        SplitImageEvenOdd(stacks[i], pack_num[i], packages);
    }
    else
      SplitImage(stacks[i], pack_num[i], packages);

    for (unsigned int j = 0; j < packages.size(); j++) {
      cout << "Package " << j << " of stack " << i << endl;
      if (_debug) {
        sprintf(buffer, "package%i-%i.nii.gz", i, j);
        packages[j].Write(buffer);
      }

      t = packages[j];
      s = _reconstructed;

      //find existing transformation
      double x, y, z;
      x = 0; y = 0; z = 0;
      packages[j].ImageToWorld(x, y, z);
      stacks[i].WorldToImage(x, y, z);

      int firstSliceIndex = round(z) + firstSlice;
      cout << "First slice index for package " << j << " of stack " << i << " is " << firstSliceIndex << endl;
      //transformation = _transformations[sliceIndex];

      //put origin in target to zero
      irtkRigidTransformation offset;
      ResetOrigin(t, offset);
      irtkMatrix mo = offset.GetMatrix();
      irtkMatrix m = _transformations[firstSliceIndex].GetMatrix();
      m = m*mo;
      _transformations[firstSliceIndex].PutMatrix(m);

      rigidregistration.SetInput(&t, &s);
      rigidregistration.SetOutput(&_transformations[firstSliceIndex]);
      rigidregistration.GuessParameterSliceToVolume();
      if (_debug)
        rigidregistration.Write("par-packages.rreg");
      rigidregistration.Run();

      //undo the offset
      mo.Invert();
      m = _transformations[firstSliceIndex].GetMatrix();
      m = m*mo;
      _transformations[firstSliceIndex].PutMatrix(m);

      if (_debug) {
        sprintf(buffer, "transformation%i-%i.dof", i, j);
        _transformations[firstSliceIndex].irtkTransformation::Write(buffer);
      }


      //set the transformation to all slices of the package
      cout << "Slices of the package " << j << " of the stack " << i << " are: ";
      for (int k = 0; k < packages[j].GetZ(); k++) {
        x = 0; y = 0; z = k;
        packages[j].ImageToWorld(x, y, z);
        stacks[i].WorldToImage(x, y, z);
        int sliceIndex = round(z) + firstSlice;
        cout << sliceIndex << " " << endl;

        if (sliceIndex >= _transformations.size()) {
          cerr << "irtkRecnstruction::PackageToVolume: sliceIndex out of range." << endl;
          cerr << sliceIndex << " " << _transformations.size() << endl;
          exit(1);
        }

        if (sliceIndex != firstSliceIndex) {
          _transformations[sliceIndex].PutTranslationX(_transformations[firstSliceIndex].GetTranslationX());
          _transformations[sliceIndex].PutTranslationY(_transformations[firstSliceIndex].GetTranslationY());
          _transformations[sliceIndex].PutTranslationZ(_transformations[firstSliceIndex].GetTranslationZ());
          _transformations[sliceIndex].PutRotationX(_transformations[firstSliceIndex].GetRotationX());
          _transformations[sliceIndex].PutRotationY(_transformations[firstSliceIndex].GetRotationY());
          _transformations[sliceIndex].PutRotationZ(_transformations[firstSliceIndex].GetRotationZ());
          _transformations[sliceIndex].UpdateMatrix();
        }
      }


    }
    cout << "End of stack " << i << endl << endl;

    firstSlice += stacks[i].GetZ();
  }
}


/* end Package specific functions */

/* Utility functions */

void irtkReconstruction::CropImage(irtkRealImage& image, irtkRealImage& mask)
{
  //Crops the image according to the mask

  int i, j, k;
  //ROI boundaries
  int x1, x2, y1, y2, z1, z2;

  //Original ROI
  x1 = 0;
  y1 = 0;
  z1 = 0;
  x2 = image.GetX();
  y2 = image.GetY();
  z2 = image.GetZ();

  //upper boundary for z coordinate
  int sum = 0;
  for (k = image.GetZ() - 1; k >= 0; k--) {
    sum = 0;
    for (j = image.GetY() - 1; j >= 0; j--)
      for (i = image.GetX() - 1; i >= 0; i--)
        if (mask.Get(i, j, k) > 0)
          sum++;
    if (sum > 0)
      break;
  }
  z2 = k;

  //lower boundary for z coordinate
  sum = 0;
  for (k = 0; k <= image.GetZ() - 1; k++) {
    sum = 0;
    for (j = image.GetY() - 1; j >= 0; j--)
      for (i = image.GetX() - 1; i >= 0; i--)
        if (mask.Get(i, j, k) > 0)
          sum++;
    if (sum > 0)
      break;
  }
  z1 = k;

  //upper boundary for y coordinate
  sum = 0;
  for (j = image.GetY() - 1; j >= 0; j--) {
    sum = 0;
    for (k = image.GetZ() - 1; k >= 0; k--)
      for (i = image.GetX() - 1; i >= 0; i--)
        if (mask.Get(i, j, k) > 0)
          sum++;
    if (sum > 0)
      break;
  }
  y2 = j;

  //lower boundary for y coordinate
  sum = 0;
  for (j = 0; j <= image.GetY() - 1; j++) {
    sum = 0;
    for (k = image.GetZ() - 1; k >= 0; k--)
      for (i = image.GetX() - 1; i >= 0; i--)
        if (mask.Get(i, j, k) > 0)
          sum++;
    if (sum > 0)
      break;
  }
  y1 = j;

  //upper boundary for x coordinate
  sum = 0;
  for (i = image.GetX() - 1; i >= 0; i--) {
    sum = 0;
    for (k = image.GetZ() - 1; k >= 0; k--)
      for (j = image.GetY() - 1; j >= 0; j--)
        if (mask.Get(i, j, k) > 0)
          sum++;
    if (sum > 0)
      break;
  }
  x2 = i;

  //lower boundary for x coordinate
  sum = 0;
  for (i = 0; i <= image.GetX() - 1; i++) {
    sum = 0;
    for (k = image.GetZ() - 1; k >= 0; k--)
      for (j = image.GetY() - 1; j >= 0; j--)
        if (mask.Get(i, j, k) > 0)
          sum++;
    if (sum > 0)
      break;
  }

  x1 = i;

  if (_debug)
    cout << "Region of interest is " << x1 << " " << y1 << " " << z1 << " " << x2 << " " << y2
    << " " << z2 << endl;

  //Cut region of interest
  image = image.GetRegion(x1, y1, z1, x2 + 1, y2 + 1, z2 + 1);
}

void irtkReconstruction::InvertStackTransformations(vector<irtkRigidTransformation>& stack_transformations)
{
  //for each stack
  for (unsigned int i = 0; i < stack_transformations.size(); i++) {
    //invert transformation for the stacks
    stack_transformations[i].Invert();
    stack_transformations[i].UpdateParameter();
  }
}


void irtkReconstruction::MaskVolumeGPU()
{
  reconstructionGPU->maskVolume();
}


void irtkReconstruction::MaskVolume()
{
  irtkRealPixel *pr = _reconstructed.GetPointerToVoxels();
  irtkRealPixel *pm = _mask.GetPointerToVoxels();
  for (int i = 0; i < _reconstructed.GetNumberOfVoxels(); i++) {
    if (*pm == 0)
      *pr = -1;
    pm++;
    pr++;
  }
}

void irtkReconstruction::MaskImage(irtkRealImage& image, double padding)
{
  if (image.GetNumberOfVoxels() != _mask.GetNumberOfVoxels()) {
    cerr << "Cannot mask the image - different dimensions" << endl;
    exit(1);
  }
  irtkRealPixel *pr = image.GetPointerToVoxels();
  irtkRealPixel *pm = _mask.GetPointerToVoxels();
  for (int i = 0; i < image.GetNumberOfVoxels(); i++) {
    if (*pm == 0)
      *pr = padding;
    pm++;
    pr++;
  }
}

/// Like PutMinMax but ignoring negative values (mask)
void irtkReconstruction::Rescale(irtkRealImage &img, double max)
{
  int i, n;
  irtkRealPixel *ptr, min_val, max_val;

  // Get lower and upper bound
  img.GetMinMax(&min_val, &max_val);

  n = img.GetNumberOfVoxels();
  ptr = img.GetPointerToVoxels();
  for (i = 0; i < n; i++)
    if (ptr[i] > 0)
      ptr[i] = double(ptr[i]) / double(max_val) * max;
}
/* end Utility functions */
