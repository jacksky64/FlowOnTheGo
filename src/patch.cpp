#include <iostream>
#include <string>
#include <vector>

#include <thread>

#include <Eigen/Core>
#include <Eigen/LU>
#include <Eigen/Dense>

#include <stdio.h>

#include <cuda_runtime.h>
#include <cublas_v2.h>
#include "common/cuda_helper.h"
#include "common/Exceptions.h"
#include "common/timer.h"

#include "patch.h"
#include "kernels/interpolate.h"


using std::cout;
using std::endl;
using std::vector;

namespace OFC {

  typedef __v4sf v4sf;

  PatClass::PatClass(
      const img_params* _i_params,
      const opt_params* _op,
      const int _patch_id)
    :
      i_params(_i_params),
      op(_op),
      patch_id(_patch_id) {

        p_state = new patch_state;

        patch.resize(op->n_vals,1);
        patch_x.resize(op->n_vals,1);
        patch_y.resize(op->n_vals,1);

        checkCudaErrors(
            cudaMalloc ((void**) &pDevicePatch, patch.size() * sizeof(float)) );
        checkCudaErrors(
            cudaMalloc ((void**) &pDevicePatchX, patch_x.size() * sizeof(float)) );
        checkCudaErrors(
            cudaMalloc ((void**) &pDevicePatchY, patch_y.size() * sizeof(float)) );
        checkCudaErrors(
            cudaMalloc ((void**) &pDeviceRawDiff, patch.size() * sizeof(float)) );
        checkCudaErrors(
            cudaMalloc ((void**) &pDeviceCostDiff, patch.size() * sizeof(float)) );
        checkCudaErrors(
            cudaMalloc ((void**) &pDeviceWeights, 4 * sizeof(float)) );
      }


  PatClass::~PatClass() {

    cudaFree(pDevicePatch);
    cudaFree(pDevicePatchX);
    cudaFree(pDevicePatchY);

    cudaFree(pDeviceRawDiff);
    cudaFree(pDeviceCostDiff);
    cudaFree(pDeviceWeights);

    delete p_state;

  }

  void PatClass::InitializePatch(const float * _I0,
      const float * _I0x, const float * _I0y, const Eigen::Vector2f _midpoint) {

    I0 = _I0;
    I0x = _I0x;
    I0y = _I0y;

    midpoint = _midpoint;

    ResetPatchState();
    ExtractPatch();
    ComputeHessian();

  }

  void PatClass::ComputeHessian() {

    CUBLAS_CHECK (
        cublasSdot(op->cublasHandle, patch.size(),
          pDevicePatchX, 1, pDevicePatchX, 1, &(p_state->hessian(0,0))) );
    CUBLAS_CHECK (
        cublasSdot(op->cublasHandle, patch.size(),
          pDevicePatchX, 1, pDevicePatchY, 1, &(p_state->hessian(0,1))) );
    CUBLAS_CHECK (
        cublasSdot(op->cublasHandle, patch.size(),
          pDevicePatchY, 1, pDevicePatchY, 1, &(p_state->hessian(1,1))) );

    p_state->hessian(1,0) = p_state->hessian(0,1);

    // If not invertible adjust values
    if (p_state->hessian.determinant() == 0) {
      p_state->hessian(0,0) += 1e-10;
      p_state->hessian(1,1) += 1e-10;
    }

  }

  void PatClass::SetTargetImage(const float * _I1) {

    I1 = _I1;

    int size = i_params->width_pad * i_params->height_pad * 3;
    checkCudaErrors(
        cudaMalloc ((void**) &pDeviceI, size *  sizeof(float)) );

    CUBLAS_CHECK (
        cublasSetVector(size, sizeof(float),
          I1, 1, pDeviceI, 1) );

    ResetPatchState();

  }

  void PatClass::ResetPatchState() {

    p_state->has_converged = 0;
    p_state->has_opt_started = 0;

    p_state->midpoint_org = midpoint;
    p_state->midpoint_cur = midpoint;

    p_state->p_org.setZero();
    p_state->p_cur.setZero();
    p_state->delta_p.setZero();

    p_state->delta_p_sq_norm = 1e-10;
    p_state->delta_p_sq_norm_init = 1e-10;
    p_state->mares = 1e20;
    p_state->mares_old = 1e20;
    p_state->count = 0;
    p_state->invalid = false;

    p_state->cost = 0.0;
  }

  void PatClass::OptimizeStart(const Eigen::Vector2f p_prev) {

    p_state->p_org = p_prev;
    p_state->p_cur = p_prev;

    UpdateMidpoint();

    // save starting location, only needed for outlier check
    p_state->midpoint_org = p_state->midpoint_cur;

    //Check if initial position is already invalid
    if (p_state->midpoint_cur[0] < i_params->l_bound
        || p_state->midpoint_cur[1] < i_params->l_bound
        || p_state->midpoint_cur[0] > i_params->u_bound_width
        || p_state->midpoint_cur[1] > i_params->u_bound_height) {

      p_state->has_converged=1;
      p_state->has_opt_started=1;

    } else {

      p_state->count = 0; // reset iteration counter
      p_state->delta_p_sq_norm = 1e-10;
      p_state->delta_p_sq_norm_init = 1e-10;  // set to arbitrary low value, s.t. that loop condition is definitely true on first iteration
      p_state->mares = 1e5;          // mean absolute residual
      p_state->mares_old = 1e20; // for rate of change, keep mares from last iteration in here. Set high so that loop condition is definitely true on first iteration
      p_state->has_converged=0;

      OptimizeComputeErrImg();

      p_state->has_opt_started = 1;
      p_state->invalid = false;

    }

  }

  void PatClass::OptimizeIter(const Eigen::Vector2f p_prev) {

    if (!p_state->has_opt_started) {

      ResetPatchState();
      OptimizeStart(p_prev);

    }

    // optimize patch until convergence, or do only one iteration if DIS visualization is used
    while (!p_state->has_converged) {

      p_state->count++;

      // Projection onto sd_images
      CUBLAS_CHECK (
          cublasSdot(op->cublasHandle, patch.size(),
            pDevicePatchX, 1, pDeviceRawDiff, 1, &(p_state->delta_p[0])) );
      CUBLAS_CHECK (
          cublasSdot(op->cublasHandle, patch.size(),
            pDevicePatchY, 1, pDeviceRawDiff, 1, &(p_state->delta_p[1])) );

      p_state->delta_p = p_state->hessian.llt().solve(p_state->delta_p); // solve linear system

      p_state->p_cur -= p_state->delta_p; // update flow vector

      // compute patch locations based on new parameter vector
      UpdateMidpoint();

      // check if patch(es) moved too far from starting location, if yes, stop iteration and reset to starting location
      // check if query patch moved more than >padval from starting location -> most likely outlier
      if ((p_state->midpoint_org - p_state->midpoint_cur).norm() > op->outlier_thresh
          || p_state->midpoint_cur[0] < i_params->l_bound
          || p_state->midpoint_cur[1] < i_params->l_bound
          || p_state->midpoint_cur[0] > i_params->u_bound_width
          || p_state->midpoint_cur[1] > i_params->u_bound_height) {

        // Reset because this is an outlier
        p_state->p_cur = p_state->p_org;
        UpdateMidpoint();
        p_state->has_converged=1;
        p_state->has_opt_started=1;

      }

      OptimizeComputeErrImg();

    }

  }

  inline void PatClass::UpdateMidpoint() {

    p_state->midpoint_cur = midpoint + p_state->p_cur;

  }

  void PatClass::ComputeCostErr() {

    // L2-Norm

    const float alpha = -1.0;

    // raw = patch - raw
    CUBLAS_CHECK (
        cublasSaxpy(op->cublasHandle, patch.size(), &alpha,
          pDevicePatch, 1, pDeviceRawDiff, 1) );

    // Element-wise multiplication
    CUBLAS_CHECK (
        cublasSdgmm(op->cublasHandle, CUBLAS_SIDE_RIGHT,
          1, patch.size(), pDeviceRawDiff, 1, pDeviceRawDiff, 1, pDeviceCostDiff, 1) );

    // Sum
    CUBLAS_CHECK (
        cublasSasum(op->cublasHandle, patch.size(),
          pDeviceCostDiff, 1, &(p_state->cost)) );

  }

  void PatClass::OptimizeComputeErrImg() {

    InterpolatePatch();
    ComputeCostErr();

    // Compute step norm
    p_state->delta_p_sq_norm = p_state->delta_p.squaredNorm();
    if (p_state->count == 1)
      p_state->delta_p_sq_norm_init = p_state->delta_p_sq_norm;

    // Check early termination criterions
    p_state->mares_old = p_state->mares;
    p_state->mares = p_state->cost / op->n_vals;

    if (!((p_state->count < op->grad_descent_iter) & (p_state->mares > op->res_thresh)
          & ((p_state->count < op->grad_descent_iter) | (p_state->delta_p_sq_norm / p_state->delta_p_sq_norm_init >= op->dp_thresh))
          & ((p_state->count < op->grad_descent_iter) | (p_state->mares / p_state->mares_old <= op->dr_thresh)))) {
      p_state->has_converged = 1;
    }

  }

  // Extract patch on integer position, and gradients, No Bilinear interpolation
  void PatClass::ExtractPatch() {

    int x = round(midpoint[0]) + i_params->padding;
    int y = round(midpoint[1]) + i_params->padding;

    int lb = -op->patch_size / 2;
    int patch_offset = 3 * ((x + lb) + (y + lb) * i_params->width_pad);

    /*float* pDeviceI0, *pDeviceI0x, *pDeviceI0y;
    int size = i_params->width_pad * i_params->height_pad * 3;
    checkCudaErrors(
        cudaMalloc ((void**) &pDeviceI0, size * sizeof(float)) );
    checkCudaErrors(
        cudaMalloc ((void**) &pDeviceI0x, size * sizeof(float)) );
    checkCudaErrors(
        cudaMalloc ((void**) &pDeviceI0y, size * sizeof(float)) );
    CUBLAS_CHECK (
        cublasSetVector(size, sizeof(float), I0, 1, pDeviceI0, 1) );
    CUBLAS_CHECK (
        cublasSetVector(size, sizeof(float), I0x, 1, pDeviceI0x, 1) );
    CUBLAS_CHECK (
        cublasSetVector(size, sizeof(float), I0y, 1, pDeviceI0y, 1) );*/

    // Extract patch
    checkCudaErrors(
        cudaMemcpy2D (pDevicePatch, 3 * op->patch_size * sizeof(float),
          I0 + patch_offset, 3 * i_params->width_pad * sizeof(float),
          3 * op->patch_size * sizeof(float), op->patch_size, cudaMemcpyDeviceToDevice) );
    checkCudaErrors(
        cudaMemcpy2D (pDevicePatchX, 3 * op->patch_size * sizeof(float),
          I0x + patch_offset, 3 * i_params->width_pad * sizeof(float),
          3 * op->patch_size * sizeof(float), op->patch_size, cudaMemcpyDeviceToDevice) );
    checkCudaErrors(
        cudaMemcpy2D (pDevicePatchY, 3 * op->patch_size * sizeof(float),
          I0y + patch_offset, 3 * i_params->width_pad * sizeof(float),
          3 * op->patch_size * sizeof(float), op->patch_size, cudaMemcpyDeviceToDevice) );

    // Mean Normalization
    if (op->use_mean_normalization > 0) {
      cu::normalizeMean(pDevicePatch, op->cublasHandle, op->patch_size);
    }

  }

  // Extract patch on float position with bilinear interpolation, no gradients.
  void PatClass::InterpolatePatch() {

    Eigen::Vector2f resid;
    Eigen::Vector4f weight; // bilinear weight vector
    Eigen::Vector4i pos;
    Eigen::Vector2i pos_iter;

    // Compute the bilinear weight vector, for patch without orientation/scale change
    // weight vector is constant for all pixels
    pos[0] = ceil(p_state->midpoint_cur[0] + .00001f); // ensure rounding up to natural numbers
    pos[1] = ceil(p_state->midpoint_cur[1] + .00001f);
    pos[2] = floor(p_state->midpoint_cur[0]);
    pos[3] = floor(p_state->midpoint_cur[1]);

    resid[0] = p_state->midpoint_cur[0] - (float)pos[2];
    resid[1] = p_state->midpoint_cur[1] - (float)pos[3];
    weight[0] = resid[0] * resid[1];
    weight[1] = (1 - resid[0]) * resid[1];
    weight[2] = resid[0] * (1- resid[1]);
    weight[3] = (1 - resid[0]) * (1 - resid[1]);

    pos[0] += i_params->padding;
    pos[1] += i_params->padding;

    checkCudaErrors(
        cudaMemcpy(pDeviceWeights, weight.data(),
          4 * sizeof(float), cudaMemcpyHostToDevice) );

    int lb = -op->patch_size / 2;
    int startx = pos[0] + lb;
    int starty = pos[1] + lb;

    cu::interpolatePatch(pDeviceRawDiff, pDeviceI, pDeviceWeights,
        i_params->width_pad, starty, startx, op->patch_size);

    // Mean Normalization
    if (op->use_mean_normalization > 0) {
      cu::normalizeMean(pDeviceRawDiff, op->cublasHandle, op->patch_size);
    }

  }

}
