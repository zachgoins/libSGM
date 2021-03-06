/*
Copyright 2016 Fixstars Corporation

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http ://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include <iostream>

#include <libsgm.h>

#include "internal.h"
#include "sgm.hpp"

namespace sgm {
	static bool is_cuda_input(EXECUTE_INOUT type) { return (int)type & 0x1; }
	static bool is_cuda_output(EXECUTE_INOUT type) { return (int)type & 0x2; }

	class SemiGlobalMatchingBase {
	public:
		using output_type = uint8_t;
		virtual void execute(output_type* dst_L, output_type* dst_R, const void* src_L, const void* src_R, 
			size_t w, size_t h, unsigned int P1, unsigned int P2, float uniqueness) = 0;

		virtual ~SemiGlobalMatchingBase() {}
	};

	template <typename input_type, int DISP_SIZE>
	class SemiGlobalMatchingImpl : public SemiGlobalMatchingBase {
	public:
		void execute(output_type* dst_L, output_type* dst_R, const void* src_L, const void* src_R,
			size_t w, size_t h, unsigned int P1, unsigned int P2, float uniqueness) override
		{
			sgm_engine_.execute(dst_L, dst_R, (const input_type*)src_L, (const input_type*)src_R, w, h, P1, P2, uniqueness);
		}
	private:
		SemiGlobalMatching<input_type, DISP_SIZE> sgm_engine_;
	};

	struct CudaStereoSGMResources {
		void* d_src_left;
		void* d_src_right;
		void* d_left_disp;
		void* d_right_disp;
		void* d_tmp_left_disp;
		void* d_tmp_right_disp;

		SemiGlobalMatchingBase* sgm_engine;

		CudaStereoSGMResources(int width_, int height_, int disparity_size_, int input_depth_bits_, int output_depth_bits_, EXECUTE_INOUT inout_type_) {

			if (input_depth_bits_ == 8 && disparity_size_ == 64)
				sgm_engine = new SemiGlobalMatchingImpl<uint8_t, 64>();
			else if (input_depth_bits_ == 8 && disparity_size_ == 128)
				sgm_engine = new SemiGlobalMatchingImpl<uint8_t, 128>();
			else if (input_depth_bits_ == 16 && disparity_size_ == 64)
				sgm_engine = new SemiGlobalMatchingImpl<uint16_t, 64>();
			else if (input_depth_bits_ == 16 && disparity_size_ == 128)
				sgm_engine = new SemiGlobalMatchingImpl<uint16_t, 128>();
			else
				throw std::logic_error("depth bits must be 8 or 16, and disparity size must be 64 or 128");

			if (is_cuda_input(inout_type_)) {
				this->d_src_left = NULL;
				this->d_src_right = NULL;
			}
			else {
				CudaSafeCall(cudaMalloc(&this->d_src_left, input_depth_bits_ / 8 * width_ * height_));
				CudaSafeCall(cudaMalloc(&this->d_src_right, input_depth_bits_ / 8 * width_ * height_));
			}
			
			CudaSafeCall(cudaMalloc(&this->d_left_disp, sizeof(uint16_t) * width_ * height_));
			CudaSafeCall(cudaMalloc(&this->d_right_disp, sizeof(uint16_t) * width_ * height_));

			CudaSafeCall(cudaMalloc(&this->d_tmp_left_disp, sizeof(uint16_t) * width_ * height_));
			CudaSafeCall(cudaMalloc(&this->d_tmp_right_disp, sizeof(uint16_t) * width_ * height_));

			CudaSafeCall(cudaMemset(this->d_left_disp, 0, sizeof(uint16_t) * width_ * height_));
			CudaSafeCall(cudaMemset(this->d_right_disp, 0, sizeof(uint16_t) * width_ * height_));
			CudaSafeCall(cudaMemset(this->d_tmp_left_disp, 0, sizeof(uint16_t) * width_ * height_));
			CudaSafeCall(cudaMemset(this->d_tmp_right_disp, 0, sizeof(uint16_t) * width_ * height_));
		}

		~CudaStereoSGMResources() {
			CudaSafeCall(cudaFree(this->d_src_left));
			CudaSafeCall(cudaFree(this->d_src_right));

			CudaSafeCall(cudaFree(this->d_left_disp));
			CudaSafeCall(cudaFree(this->d_right_disp));

			CudaSafeCall(cudaFree(this->d_tmp_left_disp));
			CudaSafeCall(cudaFree(this->d_tmp_right_disp));

			delete sgm_engine;
		}
	};

	StereoSGM::StereoSGM(int width, int height, int disparity_size, int input_depth_bits, int output_depth_bits, 
		EXECUTE_INOUT inout_type, const Parameters& param) :
		cu_res_(NULL),
		width_(width),
		height_(height),
		disparity_size_(disparity_size),
		input_depth_bits_(input_depth_bits),
		output_depth_bits_(output_depth_bits),
		inout_type_(inout_type),
		param_(param)
	{
		// check values
		if (input_depth_bits_ != 8 && input_depth_bits_ != 16 && output_depth_bits_ != 8 && output_depth_bits_ != 16) {
			width_ = height_ = input_depth_bits_ = output_depth_bits_ = disparity_size_ = 0;
			throw std::logic_error("depth bits must be 8 or 16");
		}
		if (disparity_size_ != 64 && disparity_size_ != 128) {
			width_ = height_ = input_depth_bits_ = output_depth_bits_ = disparity_size_ = 0;
			throw std::logic_error("disparity size must be 64 or 128");
		}

		cu_res_ = new CudaStereoSGMResources(width_, height_, disparity_size_, input_depth_bits_, output_depth_bits_, inout_type_);
	}

	StereoSGM::~StereoSGM() {
		if (cu_res_) { delete cu_res_; }
	}

	
	void StereoSGM::execute(const void* left_pixels, const void* right_pixels, void* dst) {

		const void *d_input_left, *d_input_right;

		if (is_cuda_input(inout_type_)) {
			d_input_left = left_pixels;
			d_input_right = right_pixels;
		}
		else {
			CudaSafeCall(cudaMemcpy(cu_res_->d_src_left, left_pixels, input_depth_bits_ / 8 * width_ * height_, cudaMemcpyHostToDevice));
			CudaSafeCall(cudaMemcpy(cu_res_->d_src_right, right_pixels, input_depth_bits_ / 8 * width_ * height_, cudaMemcpyHostToDevice));
			d_input_left = cu_res_->d_src_left;
			d_input_right = cu_res_->d_src_right;
		}

		void* d_tmp_left_disp = cu_res_->d_tmp_left_disp;
		void* d_tmp_right_disp = cu_res_->d_tmp_right_disp;
		void* d_left_disp = cu_res_->d_left_disp;
		void* d_right_disp = cu_res_->d_right_disp;

		if (is_cuda_output(inout_type_) && output_depth_bits_ == 8)
			d_left_disp = dst; // when threre is no device-host copy or type conversion, use passed buffer
		
		cu_res_->sgm_engine->execute((uint8_t*)d_tmp_left_disp, (uint8_t*)d_tmp_right_disp,
			d_input_left, d_input_right, width_, height_, param_.P1, param_.P2, param_.uniqueness);

		sgm::details::median_filter((uint8_t*)d_tmp_left_disp, (uint8_t*)d_left_disp, width_, height_);
		sgm::details::median_filter((uint8_t*)d_tmp_right_disp, (uint8_t*)d_right_disp, width_, height_);
		sgm::details::check_consistency((uint8_t*)d_left_disp, (uint8_t*)d_right_disp, d_input_left, width_, height_, input_depth_bits_);

		if (!is_cuda_output(inout_type_) && output_depth_bits_ == 16) {
			sgm::details::cast_8bit_16bit_array((const uint8_t*)d_left_disp, (uint16_t*)d_tmp_left_disp, width_ * height_);
			CudaSafeCall(cudaMemcpy(dst, d_tmp_left_disp, sizeof(uint16_t) * width_ * height_, cudaMemcpyDeviceToHost));
		}
		else if (is_cuda_output(inout_type_) && output_depth_bits_ == 16) {
			sgm::details::cast_8bit_16bit_array((const uint8_t*)d_left_disp, (uint16_t*)dst, width_ * height_);
		}
		else if (!is_cuda_output(inout_type_) && output_depth_bits_ == 8) {
			CudaSafeCall(cudaMemcpy(dst, d_left_disp, sizeof(uint8_t) * width_ * height_, cudaMemcpyDeviceToHost));
		}
		else if (is_cuda_output(inout_type_) && output_depth_bits_ == 8) {
			// optimize! no-copy!
		}
		else {
			std::cerr << "not impl" << std::endl;
		}
	}
}
