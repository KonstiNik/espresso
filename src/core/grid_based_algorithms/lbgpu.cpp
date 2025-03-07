/*
 * Copyright (C) 2010-2022 The ESPResSo project
 *
 * This file is part of ESPResSo.
 *
 * ESPResSo is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * ESPResSo is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
/** \file
 *  %Lattice Boltzmann on GPUs.
 *
 *  The corresponding header file is lbgpu.hpp.
 */

#include "config/config.hpp"

#ifdef CUDA

#include "lbgpu.hpp"

#include "communication.hpp"
#include "cuda_interface.hpp"
#include "errorhandling.hpp"
#include "grid.hpp"
#include "integrate.hpp"
#include "lb-d3q19.hpp"

#include <utils/math/sqr.hpp>

#include <cmath>
#include <limits>
#include <vector>

LB_parameters_gpu lbpar_gpu = {
    // rho
    0.f,
    // mu
    0.f,
    // viscosity
    0.f,
    // gamma_shear
    0.f,
    // gamma_bulk
    0.f,
    // gamma_odd
    0.f,
    // gamma_even
    0.f,
    // is_TRT
    false,
    // bulk_viscosity
    -1.f,
    // agrid
    -1.f,
    // tau
    -1.f,
    // dim
    {{{0u, 0u, 0u}}},
    // number_of_nodes
    0u,
#ifdef LB_BOUNDARIES_GPU
    // number_of_boundnodes
    0u,
#endif
    // external_force_density
    false,
    // ext_force_density
    {{{0.f, 0.f, 0.f}}},
    // Thermal energy
    0.f};

/** this is the array that stores the hydrodynamic fields for the output */
std::vector<LB_rho_v_pi_gpu> host_values(0);

#ifdef ELECTROKINETICS
bool ek_initialized = false;
#endif

/** (Re-)initialize the fluid according to the given value of rho. */
void lb_reinit_fluid_gpu() {
  lb_reinit_parameters_gpu();
  if (lbpar_gpu.number_of_nodes != 0u) {
    lb_reinit_GPU(&lbpar_gpu);
    lb_reinit_extern_nodeforce_GPU(&lbpar_gpu);
  }
}

/** (Re-)initialize the fluid.
 *  See @cite dunweg07a and @cite dhumieres09a.
 */
void lb_reinit_parameters_gpu() {
  lbpar_gpu.mu = 0.f;

  if (lbpar_gpu.viscosity > 0.f && lbpar_gpu.agrid > 0.f &&
      lbpar_gpu.tau > 0.f) {
    /* Eq. (80) @cite dunweg07a. */
    lbpar_gpu.gamma_shear = 1.f - 2.f / (6.f * lbpar_gpu.viscosity + 1.f);
  }

  if (lbpar_gpu.bulk_viscosity > 0.f) {
    /* Eq. (81) @cite dunweg07a. */
    lbpar_gpu.gamma_bulk = 1.f - 2.f / (9.f * lbpar_gpu.bulk_viscosity + 1.f);
  }

  // By default, gamma_even and gamma_odd are chosen such that the MRT becomes
  // a TRT with ghost mode relaxation factors that minimize unphysical wall
  // slip at bounce-back boundaries. For the relation between the gammas
  // achieving this, consult @cite dhumieres09a.
  // Note that the relaxation operator in ESPResSo is defined as
  //  m* = m_eq + gamma * (m - m_eq)
  // as opposed to this reference, where
  //  m* = m + lambda * (m - m_eq)

  if (lbpar_gpu.is_TRT) {
    lbpar_gpu.gamma_bulk = lbpar_gpu.gamma_shear;
    lbpar_gpu.gamma_even = lbpar_gpu.gamma_shear;
    lbpar_gpu.gamma_odd =
        -(7.f * lbpar_gpu.gamma_even + 1.f) / (lbpar_gpu.gamma_even + 7.f);
  }

  if (lbpar_gpu.kT > 0.f) { /* fluctuating hydrodynamics ? */

    /* Eq. (51) @cite dunweg07a.*/
    /* Note that the modes are not normalized as in the paper here! */
    lbpar_gpu.mu = lbpar_gpu.kT * Utils::sqr(lbpar_gpu.tau) /
                   D3Q19::c_sound_sq<float> / Utils::sqr(lbpar_gpu.agrid);
  }

  lb_set_agrid_gpu(lbpar_gpu.agrid);

#ifdef ELECTROKINETICS
  if (ek_initialized) {
    lbpar_gpu.tau = static_cast<float>(get_time_step());
  }
#endif

  reinit_parameters_GPU(&lbpar_gpu);
}

/** Performs a full initialization of the lattice Boltzmann system.
 *  All derived parameters and the fluid are reset to their default values.
 */
void lb_init_gpu_local() {
  if (this_node == 0) {
    /* set parameters for transfer to gpu */
    lb_reinit_parameters_gpu();
    lb_init_GPU(lbpar_gpu);
  }
  gpu_init_particle_comm(this_node);
  cuda_bcast_global_part_params();
}

REGISTER_CALLBACK(lb_init_gpu_local)

void lb_init_gpu() { mpi_call_all(lb_init_gpu_local); }

void lb_GPU_sanity_checks() {
  if (this_node == 0) {
    if (lbpar_gpu.agrid < 0.f) {
      runtimeErrorMsg() << "Lattice Boltzmann agrid not set";
    }
    if (lbpar_gpu.tau < 0.f) {
      runtimeErrorMsg() << "Lattice Boltzmann time step not set";
    }
    if (lbpar_gpu.rho < 0.f) {
      runtimeErrorMsg() << "Lattice Boltzmann fluid density not set";
    }
    if (lbpar_gpu.viscosity < 0.f) {
      runtimeErrorMsg() << "Lattice Boltzmann fluid viscosity not set";
    }
  }
}

void lb_set_agrid_gpu(double agrid) {
  lbpar_gpu.agrid = static_cast<float>(agrid);

  lbpar_gpu.dim[0] =
      static_cast<unsigned int>(round(box_geo.length()[0] / agrid));
  lbpar_gpu.dim[1] =
      static_cast<unsigned int>(round(box_geo.length()[1] / agrid));
  lbpar_gpu.dim[2] =
      static_cast<unsigned int>(round(box_geo.length()[2] / agrid));

  Utils::Vector<float, 3> box_from_dim(
      Utils::Vector<unsigned int, 3>(lbpar_gpu.dim) * agrid);
  Utils::Vector<float, 3> box_lf(box_geo.length());

  auto const rel_difference_vec =
      Utils::hadamard_division(box_lf - box_from_dim, box_lf);
  auto const commensurable = std::all_of(
      rel_difference_vec.begin(), rel_difference_vec.end(), [](auto d) {
        return std::abs(d) < std::numeric_limits<float>::epsilon();
      });
  if (not commensurable) {
    runtimeErrorMsg() << "Lattice spacing agrid=" << agrid
                      << " is incompatible with one of the box dimensions: "
                      << "[" << box_geo.length() << "]";
  }
  lbpar_gpu.number_of_nodes =
      std::accumulate(lbpar_gpu.dim.begin(), lbpar_gpu.dim.end(), 1u,
                      std::multiplies<unsigned int>());
}

#endif // CUDA
