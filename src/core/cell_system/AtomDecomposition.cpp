/*
 * Copyright (C) 2010-2022 The ESPResSo project
 * Copyright (C) 2002,2003,2004,2005,2006,2007,2008,2009,2010
 *   Max-Planck-Institute for Polymer Research, Theory Group
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

#include "cell_system/AtomDecomposition.hpp"

#include "cell_system/Cell.hpp"

#include <utils/Vector.hpp>

#include <boost/mpi/collectives/all_to_all.hpp>

#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

void AtomDecomposition::configure_neighbors() {
  std::vector<Cell *> red_neighbors;
  std::vector<Cell *> black_neighbors;

  /* distribute force calculation work  */
  for (int n = 0; n < m_comm.size(); n++) {
    if (m_comm.rank() == n) {
      continue;
    }

    if (n < m_comm.rank()) {
      red_neighbors.push_back(&cells.at(n));
    } else {
      black_neighbors.push_back(&cells.at(n));
    }
  }

  local().m_neighbors = Neighbors<Cell *>(red_neighbors, black_neighbors);
}
GhostCommunicator AtomDecomposition::prepare_comm() {
  /* no need for comm for only 1 node */
  if (m_comm.size() == 1) {
    return GhostCommunicator{m_comm, 0};
  }

  auto ghost_comm =
      GhostCommunicator{m_comm, static_cast<std::size_t>(m_comm.size())};
  /* every node has its dedicated comm step */
  for (int n = 0; n < m_comm.size(); n++) {
    ghost_comm.communications[n].part_lists.resize(1);
    ghost_comm.communications[n].part_lists[0] = &(cells.at(n).particles());
    ghost_comm.communications[n].node = n;
  }

  return ghost_comm;
}
void AtomDecomposition::configure_comms() {
  m_exchange_ghosts_comm = prepare_comm();
  m_collect_ghost_force_comm = prepare_comm();

  if (m_comm.size() > 1) {
    for (int n = 0; n < m_comm.size(); n++) {
      /* use the prefetched send buffers. Node 0 transmits first and never
       * prefetches. */
      if (m_comm.rank() == 0 || m_comm.rank() != n) {
        m_exchange_ghosts_comm.communications[n].type = GHOST_BCST;
      } else {
        m_exchange_ghosts_comm.communications[n].type =
            GHOST_BCST | GHOST_PREFETCH;
      }
      m_collect_ghost_force_comm.communications[n].type = GHOST_RDCE;
    }
    /* first round: all nodes except the first one prefetch their send data */
    if (m_comm.rank() != 0) {
      m_exchange_ghosts_comm.communications[0].type |= GHOST_PREFETCH;
    }
  }
}
void AtomDecomposition::mark_cells() {
  m_local_cells.resize(1, std::addressof(local()));
  m_ghost_cells.clear();
  for (int n = 0; n < m_comm.size(); n++) {
    if (n != m_comm.rank()) {
      m_ghost_cells.push_back(std::addressof(cells.at(n)));
    }
  }
}
void AtomDecomposition::resort(bool global_flag,
                               std::vector<ParticleChange> &diff) {
  for (auto &p : local().particles()) {
    fold_position(p.pos(), p.image_box(), m_box);

    p.pos_at_last_verlet_update() = p.pos();
  }

  /* Local updates are a NoOp for this decomposition. */
  if (not global_flag) {
    return;
  }

  /* Sort displaced particles by the node they belong to. */
  std::vector<std::vector<Particle>> send_buf(m_comm.size());
  for (auto it = local().particles().begin();
       it != local().particles().end();) {
    auto const target_node = id_to_rank(it->id());
    if (target_node != m_comm.rank()) {
      diff.emplace_back(RemovedParticle{it->id()});
      send_buf.at(target_node).emplace_back(std::move(*it));
      it = local().particles().erase(it);
    } else {
      ++it;
    }
  }

  /* Exchange particles */
  std::vector<std::vector<Particle>> recv_buf(m_comm.size());
  boost::mpi::all_to_all(m_comm, send_buf, recv_buf);

  diff.emplace_back(ModifiedList{local().particles()});

  /* Add new particles belonging to this node */
  for (auto &parts : recv_buf) {
    for (auto &p : parts) {
      local().particles().insert(std::move(p));
    }
  }
}

AtomDecomposition::AtomDecomposition(BoxGeometry const &box_geo)
    : m_box(box_geo) {}

AtomDecomposition::AtomDecomposition(boost::mpi::communicator comm,
                                     BoxGeometry const &box_geo)
    : m_comm(std::move(comm)), cells(m_comm.size()), m_box(box_geo) {
  /* create communicators */
  configure_comms();
  /* configure neighbor relations */
  configure_neighbors();
  /* fill local and ghost cell lists */
  mark_cells();
}

Utils::Vector3d AtomDecomposition::max_cutoff() const {
  return Utils::Vector3d::broadcast(std::numeric_limits<double>::infinity());
}

Utils::Vector3d AtomDecomposition::max_range() const { return max_cutoff(); }
