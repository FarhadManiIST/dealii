/* ---------------------------------------------------------------------
 *
 * Copyright (C) 2018 by the deal.II authors
 *
 * This file is part of the deal.II library.
 *
 * The deal.II library is free software; you can use it, redistribute
 * it, and/or modify it under the terms of the GNU Lesser General
 * Public License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * The full text of the license can be found in the file LICENSE.md at
 * the top level directory of deal.II.
 *
 * ---------------------------------------------------------------------
 */

// a variation of periodicity_06 that used to trigger
// AffineConstraints::is_consistent_in_parallel() on 13 mpi tasks.

#include <deal.II/base/conditional_ostream.h>

#include <deal.II/distributed/tria.h>

#include <deal.II/dofs/dof_tools.h>

#include <deal.II/fe/fe_q.h>
#include <deal.II/fe/fe_system.h>
#include <deal.II/fe/mapping_q1.h>

#include <deal.II/grid/grid_generator.h>
#include <deal.II/grid/grid_tools.h>

#include <deal.II/numerics/data_out.h>

#include "../tests.h"

template <int dim>
void
test(const unsigned numRefinementLevels = 2)
{
  MPI_Comm mpi_communicator = MPI_COMM_WORLD;

  const unsigned int n_mpi_processes =
    Utilities::MPI::n_mpi_processes(mpi_communicator);
  const unsigned int this_mpi_process =
    Utilities::MPI::this_mpi_process(mpi_communicator);

  ConditionalOStream pcout(std::cout, this_mpi_process == 0);

  const double                                      L = 20;
  dealii::parallel::distributed::Triangulation<dim> triangulation(
    mpi_communicator);
  GridGenerator::hyper_cube(triangulation, -L, L, /*colorize*/ false);

  // mark faces
  for (auto cell : triangulation.active_cell_iterators())
    for (unsigned int f = 0; f < GeometryInfo<dim>::faces_per_cell; ++f)
      {
        const Point<dim> &face_center = cell->face(f)->center();
        if (cell->face(f)->at_boundary())
          {
            unsigned int counter = 1;
            for (unsigned int d = 0; d < dim; ++d)
              {
                if (std::abs(face_center[d] - L) < 1.0e-5)
                  cell->face(f)->set_boundary_id(counter);
                ++counter;
                if (std::abs(face_center[d] + L) < 1.0e-5)
                  cell->face(f)->set_boundary_id(counter);
                ++counter;
              }
          }
      }

  std::vector<
    GridTools::PeriodicFacePair<typename Triangulation<dim>::cell_iterator>>
    periodicity_vector;
  for (int d = 0; d < dim; ++d)
    GridTools::collect_periodic_faces(triangulation,
                                      /*b_id1*/ 2 * d + 1,
                                      /*b_id2*/ 2 * d + 2,
                                      /*direction*/ d,
                                      periodicity_vector);

  triangulation.add_periodicity(periodicity_vector);

  // refine mesh
  triangulation.refine_global(1);

  Point<dim> corner;
  for (unsigned int d = 0; d < dim; ++d)
    corner[d] = -L;

  MappingQ1<dim> mapping;
  for (unsigned int ilevel = 0; ilevel < numRefinementLevels; ilevel++)
    {
      // pick an corner cell and refine
      for (auto cell : triangulation.active_cell_iterators())
        {
          try
            {
              const Point<dim> p_cell =
                mapping.transform_real_to_unit_cell(cell, corner);
              const double dist =
                GeometryInfo<dim>::distance_to_unit_cell(p_cell);

              if (dist < 1e-08)
                cell->set_refine_flag();
            }
          catch (typename MappingQ1<dim>::ExcTransformationFailed)
            {}
        }
      triangulation.execute_coarsening_and_refinement();
    }

  pcout << "number of elements: " << triangulation.n_global_active_cells()
        << std::endl;

  // create dofHandler
  FESystem<dim>   FE(FE_Q<dim>(QGaussLobatto<1>(2)), 1);
  DoFHandler<dim> dofHandler(triangulation);
  dofHandler.distribute_dofs(FE);

  // write mesh for visualization
  DataOut<dim> data_out;
  data_out.attach_dof_handler(dofHandler);
  Vector<float> subdomain(triangulation.n_active_cells());
  for (unsigned int i = 0; i < subdomain.size(); ++i)
    subdomain(i) = triangulation.locally_owned_subdomain();
  data_out.add_data_vector(subdomain, "subdomain");
  data_out.build_patches();
  data_out.write_vtu_in_parallel(std::string("mesh.vtu").c_str(),
                                 mpi_communicator);

  IndexSet locally_relevant_dofs;
  DoFTools::extract_locally_relevant_dofs(dofHandler, locally_relevant_dofs);

  IndexSet locally_active_dofs;
  DoFTools::extract_locally_active_dofs(dofHandler, locally_active_dofs);

  const std::vector<IndexSet> &locally_owned_dofs =
    dofHandler.locally_owned_dofs_per_processor();

  std::map<types::global_dof_index, Point<dim>> supportPoints;
  DoFTools::map_dofs_to_support_points(MappingQ1<dim>(),
                                       dofHandler,
                                       supportPoints);

  /// creating combined hanging node and periodic constraint matrix
  AffineConstraints<double> constraints;
  constraints.clear();
  constraints.reinit(locally_relevant_dofs);
  DoFTools::make_hanging_node_constraints(dofHandler, constraints);

  const bool hanging_consistent =
    constraints.is_consistent_in_parallel(locally_owned_dofs,
                                          locally_active_dofs,
                                          mpi_communicator);

  pcout << "Hanging nodes constraints are consistent in parallel: "
        << hanging_consistent << std::endl;

  std::vector<
    GridTools::PeriodicFacePair<typename DoFHandler<dim>::cell_iterator>>
    periodicity_vectorDof;
  for (int d = 0; d < dim; ++d)
    GridTools::collect_periodic_faces(dofHandler,
                                      /*b_id1*/ 2 * d + 1,
                                      /*b_id2*/ 2 * d + 2,
                                      /*direction*/ d,
                                      periodicity_vectorDof);

  DoFTools::make_periodicity_constraints<DoFHandler<dim>>(periodicity_vectorDof,
                                                          constraints);
  constraints.close();

  const bool consistent =
    constraints.is_consistent_in_parallel(locally_owned_dofs,
                                          locally_active_dofs,
                                          mpi_communicator,
                                          /*verbose*/ true);

  pcout << "Total constraints are consistent in parallel: " << consistent
        << std::endl;

  /* verbose output of is_consistent_in_parallel() gives:

  Proc 10 got line 370 from 11 wrong values!
  Proc 10 got line 374 from 11 wrong values!
  Proc 10 got line 378 from 11 wrong values!
  3 inconsistent lines discovered!

  */

  const std::vector<unsigned int> wrong_lines = {{370, 374, 378}};

  for (unsigned int i = 0; i < n_mpi_processes; ++i)
    {
      if (this_mpi_process == i)
        {
          std::cout << "=== Process " << i << std::endl;
          // constraints.print(std::cout);
          // std::cout << "Owned DoFs:" << std::endl;
          // dofHandler.locally_owned_dofs().print(std::cout);
          // std::cout << "Ghost DoFs:" << std::endl;
          // locally_relevant_dofs.print(std::cout);

          for (auto ind : wrong_lines)
            if (locally_relevant_dofs.is_element(ind) &&
                constraints.is_constrained(ind))
              {
                std::cout << "Constraints for " << ind << " @ "
                          << supportPoints[ind] << ":" << std::endl;
                for (auto c : *constraints.get_constraint_entries(ind))
                  std::cout << "    " << c.first << " @ "
                            << supportPoints[c.first] << " :  " << c.second
                            << std::endl;
              }
        }
      MPI_Barrier(mpi_communicator);
    }
}

int
main(int argc, char *argv[])
{
  Utilities::MPI::MPI_InitFinalize mpi_initialization(argc, argv);
  test<3>(4);
}
