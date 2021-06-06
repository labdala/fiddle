#include <fiddle/base/samrai_utilities.h>

#include <fiddle/grid/box_utilities.h>

#include <fiddle/interaction/elemental_interaction.h>
#include <fiddle/interaction/ifed_method.h>
#include <fiddle/interaction/interaction_utilities.h>

#include <deal.II/distributed/shared_tria.h>

#include <deal.II/fe/mapping_fe_field.h>

#include <ibamr/IBHierarchyIntegrator.h>

#include <CellVariable.h>
#include <HierarchyDataOpsManager.h>
#include <IntVector.h>
#include <VariableDatabase.h>

#include <deque>

namespace fdl
{
  using namespace dealii;
  using namespace SAMRAI;

  template <int dim, int spacedim>
  IFEDMethod<dim, spacedim>::IFEDMethod(
    tbox::Pointer<tbox::Database>      input_db,
    std::vector<Part<dim, spacedim>> &&input_parts)
    : input_db(input_db)
    , parts(std::move(input_parts))
    , secondary_hierarchy("ifed::secondary_hierarchy",
                          input_db->getDatabase("GriddingAlgorithm"),
                          input_db->getDatabase("LoadBalancer"))
  {
    // IBFEMethod uses this value - lower values aren't guaranteed to work. If
    // dx = dX then we can use a lower density.
    const double density =
      input_db->getDoubleWithDefault("IB_point_density", 2.0);
    for (unsigned int part_n = 0; part_n < parts.size(); ++part_n)
      {
        const unsigned int n_points_1D =
          parts[part_n].get_dof_handler().get_fe().tensor_degree() + 1;
        interactions.emplace_back(
          new ElementalInteraction<dim, spacedim>(n_points_1D, density));
      }
  }

  template <int dim, int spacedim>
  void
  IFEDMethod<dim, spacedim>::initializePatchHierarchy(
    tbox::Pointer<hier::PatchHierarchy<spacedim>> hierarchy,
    tbox::Pointer<mesh::GriddingAlgorithm<spacedim>> /*gridding_alg*/,
    int /*u_data_index*/,
    const std::vector<tbox::Pointer<xfer::CoarsenSchedule<spacedim>>>
      & /*u_synch_scheds*/,
    const std::vector<tbox::Pointer<xfer::RefineSchedule<spacedim>>>
      & /*u_ghost_fill_scheds*/,
    int /*integrator_step*/,
    double /*init_data_time*/,
    bool /*initial_time*/)
  {
    primary_hierarchy = hierarchy;

    primary_eulerian_data_cache = std::make_shared<IBTK::SAMRAIDataCache>();
    primary_eulerian_data_cache->setPatchHierarchy(hierarchy);
    primary_eulerian_data_cache->resetLevels(0,
                                             hierarchy->getFinestLevelNumber());

    secondary_hierarchy.reinit(primary_hierarchy->getFinestLevelNumber(),
                               primary_hierarchy->getFinestLevelNumber(),
                               primary_hierarchy);

    reinit_interactions();
  }

  template <int dim, int spacedim>
  void
  IFEDMethod<dim, spacedim>::interpolateVelocity(
    int u_data_index,
    const std::vector<tbox::Pointer<xfer::CoarsenSchedule<spacedim>>>
      &u_synch_scheds,
    const std::vector<tbox::Pointer<xfer::RefineSchedule<spacedim>>>
      &    u_ghost_fill_scheds,
    double data_time)
  {
    (void)u_synch_scheds;
    (void)u_ghost_fill_scheds;

    // Update the secondary hierarchy:
    secondary_hierarchy
      .getPrimaryToScratchSchedule(primary_hierarchy->getFinestLevelNumber(),
                                   u_data_index,
                                   u_data_index,
                                   d_ib_solver->getVelocityPhysBdryOp())
      .fillData(data_time);

    std::vector<std::unique_ptr<TransactionBase>> transactions;
    // we emplace_back so use a deque to keep pointers valid
    std::deque<LinearAlgebra::distributed::Vector<double>> F_rhs_vecs;

    // start:
    for (unsigned int part_n = 0; part_n < parts.size(); ++part_n)
      {
        const Part<dim, spacedim> &part = parts[part_n];
        F_rhs_vecs.emplace_back(part.get_partitioner());
        transactions.emplace_back(
          interactions[part_n]->compute_projection_rhs_start(
            u_data_index,
            part.get_dof_handler(),
            get_position(part_n, data_time),
            part.get_dof_handler(),
            part.get_mapping(),
            F_rhs_vecs[part_n]));
      }

    // Compute:
    for (unsigned int part_n = 0; part_n < parts.size(); ++part_n)
      transactions[part_n] =
        interactions[part_n]->compute_projection_rhs_intermediate(
          std::move(transactions[part_n]));

    // Collect:
    for (unsigned int part_n = 0; part_n < parts.size(); ++part_n)
      interactions[part_n]->compute_projection_rhs_finish(
        std::move(transactions[part_n]));

    // Project:
    for (unsigned int part_n = 0; part_n < parts.size(); ++part_n)
      {
        SolverControl control(1000, 1e-14 * F_rhs_vecs[part_n].l2_norm());
        SolverCG<LinearAlgebra::distributed::Vector<double>> cg(control);
        LinearAlgebra::distributed::Vector<double>           velocity(
          parts[part_n].get_partitioner());
        // TODO - implement initial guess stuff here
        cg.solve(parts[part_n].get_mass_operator(),
                 velocity,
                 F_rhs_vecs[part_n],
                 parts[part_n].get_mass_preconditioner());
        if (std::abs(data_time - half_time) < 1e-14)
          {
            half_velocity_vectors.resize(parts.size());
            half_velocity_vectors[part_n] = std::move(velocity);
          }
        else if (std::abs(data_time - new_time) < 1e-14)
          {
            new_velocity_vectors.resize(parts.size());
            new_velocity_vectors[part_n] = std::move(velocity);
          }
        else
          Assert(false, ExcFDLNotImplemented());
      }
  }



  template <int dim, int spacedim>
  void
  IFEDMethod<dim, spacedim>::spreadForce(
    int                               f_data_index,
    IBTK::RobinPhysBdryPatchStrategy *f_phys_bdry_op,
    const std::vector<tbox::Pointer<xfer::RefineSchedule<spacedim>>>
      & /*f_prolongation_scheds*/,
    double data_time)
  {
    const int level_number = primary_hierarchy->getFinestLevelNumber();

    std::shared_ptr<IBTK::SAMRAIDataCache> data_cache =
      secondary_hierarchy.getSAMRAIDataCache();
    auto       hierarchy = secondary_hierarchy.d_secondary_hierarchy;
    const auto f_scratch_data_index =
      data_cache->getCachedPatchDataIndex(f_data_index);
    fill_all(hierarchy, f_scratch_data_index, level_number, level_number, 0.0);

    // start:
    std::vector<std::unique_ptr<TransactionBase>> transactions;
    for (unsigned int part_n = 0; part_n < parts.size(); ++part_n)
      {
        const Part<dim, spacedim> &part = parts[part_n];
        transactions.emplace_back(interactions[part_n]->compute_spread_start(
          f_scratch_data_index,
          get_position(part_n, data_time),
          part.get_dof_handler(),
          part.get_mapping(),
          part.get_dof_handler(),
          get_force(part_n, data_time)));
      }

    // Compute:
    for (unsigned int part_n = 0; part_n < parts.size(); ++part_n)
      transactions[part_n] = interactions[part_n]->compute_spread_intermediate(
        std::move(transactions[part_n]));

    // Collect:
    for (unsigned int part_n = 0; part_n < parts.size(); ++part_n)
      interactions[part_n]->compute_spread_finish(
        std::move(transactions[part_n]));

    // Deal with force values spread outside the physical domain. Since these
    // are spread into ghost regions that don't correspond to actual degrees
    // of freedom they are ignored by the accumulation step - we have to
    // handle this before we do that.
    if (f_phys_bdry_op)
      {
        f_phys_bdry_op->setPatchDataIndex(f_scratch_data_index);
        tbox::Pointer<hier::PatchLevel<spacedim>> level =
          hierarchy->getPatchLevel(level_number);
        for (typename hier::PatchLevel<spacedim>::Iterator p(level); p; p++)
          {
            const tbox::Pointer<hier::Patch<spacedim>> patch =
              level->getPatch(p());
            tbox::Pointer<hier::PatchData<spacedim>> f_data =
              patch->getPatchData(f_scratch_data_index);
            f_phys_bdry_op->accumulateFromPhysicalBoundaryData(
              *patch, data_time, f_data->getGhostCellWidth());
          }
      }

    tbox::Pointer<hier::Variable<spacedim>> f_var;
    hier::VariableDatabase<spacedim>::getDatabase()->mapIndexToVariable(
      f_data_index, f_var);
    // Accumulate forces spread into patch ghost regions.
    {
      if (!ghost_data_accumulator)
        {
          // If we have multiple IBMethod objects we may end up with a wider
          // ghost region than the one required by this class. Hence, set the
          // ghost width by just picking whatever the data actually has at the
          // moment.
          const tbox::Pointer<hier::PatchLevel<spacedim>> level =
            hierarchy->getPatchLevel(level_number);
          const hier::IntVector<spacedim> gcw =
            level->getPatchDescriptor()
              ->getPatchDataFactory(f_scratch_data_index)
              ->getGhostCellWidth();

          ghost_data_accumulator.reset(new IBTK::SAMRAIGhostDataAccumulator(
            hierarchy, f_var, gcw, level_number, level_number));
        }
      ghost_data_accumulator->accumulateGhostData(f_scratch_data_index);
    }

    // Sum values back into the primary hierarchy.
    {
      auto f_primary_data_ops =
        extract_hierarchy_data_ops(f_var, primary_hierarchy);
      f_primary_data_ops->resetLevels(level_number, level_number);
      const auto f_primary_scratch_data_index =
        primary_eulerian_data_cache->getCachedPatchDataIndex(f_data_index);
      // we have to zero everything here since the scratch to primary
      // communication does not touch ghost cells, which may have junk
      fill_all(primary_hierarchy,
               f_primary_scratch_data_index,
               level_number,
               level_number,
               0.0);
      secondary_hierarchy
        .getScratchToPrimarySchedule(level_number,
                                     f_primary_scratch_data_index,
                                     f_scratch_data_index)
        .fillData(data_time);
      f_primary_data_ops->add(f_data_index,
                              f_data_index,
                              f_primary_scratch_data_index);
    }
  }



  template <int dim, int spacedim>
  void
  IFEDMethod<dim, spacedim>::applyGradientDetector(
    tbox::Pointer<hier::BasePatchHierarchy<spacedim>> hierarchy,
    int                                               level_number,
    double /*error_data_time*/,
    int tag_index,
    bool /*initial_time*/,
    bool /*uses_richardson_extrapolation_too*/)
  {
    // TODO: we should find a way to save the bboxes so they do not need to be
    // computed for each level that needs tagging - conceivably this could
    // happen in beginDataRedistribution() and the array can be cleared in
    // endDataRedistribution()
    for (const Part<dim, spacedim> &part : parts)
      {
        const DoFHandler<dim, spacedim> &dof_handler = part.get_dof_handler();
        MappingFEField<dim,
                       spacedim,
                       LinearAlgebra::distributed::Vector<double>>
                   mapping(dof_handler, part.get_position());
        const auto local_bboxes =
          compute_cell_bboxes<dim, spacedim, float>(dof_handler, mapping);
        // Like most other things this only works with p::S::T now
        const auto &tria =
          dynamic_cast<const parallel::shared::Triangulation<dim, spacedim> &>(
            part.get_triangulation());
        const auto global_bboxes =
          collect_all_active_cell_bboxes(tria, local_bboxes);
        tbox::Pointer<hier::PatchLevel<spacedim>> patch_level =
          hierarchy->getPatchLevel(level_number);
        Assert(patch_level, ExcNotImplemented());
        tag_cells(global_bboxes, tag_index, patch_level);
      }
  }

  template <int dim, int spacedim>
  void IFEDMethod<dim, spacedim>::beginDataRedistribution(
    tbox::Pointer<hier::PatchHierarchy<spacedim>> /*hierarchy*/,
    tbox::Pointer<mesh::GriddingAlgorithm<spacedim>> /*gridding_alg*/)
  {
    // This function is called before initializePatchHierarchy is - in that case
    // we don't have a hierarchy, so we don't have any data, and there is naught
    // to do
    if (primary_hierarchy)
      {
        // TODO - calculate a nonzero workload using the secondary hierarchy
        const int ln = primary_hierarchy->getFinestLevelNumber();
        tbox::Pointer<hier::PatchLevel<spacedim>> level =
          primary_hierarchy->getPatchLevel(ln);
        if (!level->checkAllocated(lagrangian_workload_current_index))
          level->allocatePatchData(lagrangian_workload_current_index);

        auto ops = extract_hierarchy_data_ops(lagrangian_workload_var,
                                              primary_hierarchy);
        ops->resetLevels(ln, ln);
        ops->setToScalar(lagrangian_workload_current_index, 0.0);
      }

    // Clear a few things that depend on the current hierarchy:
    ghost_data_accumulator.reset();
  }

  template <int dim, int spacedim>
  void IFEDMethod<dim, spacedim>::endDataRedistribution(
    tbox::Pointer<hier::PatchHierarchy<spacedim>> /*hierarchy*/,
    tbox::Pointer<mesh::GriddingAlgorithm<spacedim>> /*gridding_alg*/)
  {
    // same as beginDataRedistribution
    if (primary_hierarchy)
      {
        secondary_hierarchy.reinit(primary_hierarchy->getFinestLevelNumber(),
                                   primary_hierarchy->getFinestLevelNumber(),
                                   primary_hierarchy,
                                   lagrangian_workload_current_index);

        reinit_interactions();
      }
  }



  template <int dim, int spacedim>
  void
  IFEDMethod<dim, spacedim>::registerEulerianVariables()
  {
    // we need ghosts for CONSERVATIVE_LINEAR_REFINE
    const hier::IntVector<spacedim> ghosts = 1;
    lagrangian_workload_var =
      new pdat::CellVariable<spacedim, double>("::lagrangian_workload");
    registerVariable(lagrangian_workload_current_index,
                     lagrangian_workload_new_index,
                     lagrangian_workload_scratch_index,
                     lagrangian_workload_var,
                     ghosts,
                     "CONSERVATIVE_COARSEN",
                     "CONSERVATIVE_LINEAR_REFINE");
  }


  template <int dim, int spacedim>
  const hier::IntVector<spacedim> &
  IFEDMethod<dim, spacedim>::getMinimumGhostCellWidth() const
  {
    // Like elsewhere, we are hard-coding in bspline 3 for now
    const std::string kernel_name = "BSPLINE_3";
    const int         ghost_width =
      IBTK::LEInteractor::getMinimumGhostWidth(kernel_name);
    static hier::IntVector<spacedim> gcw;
    for (int i = 0; i < spacedim; ++i)
      gcw[i] = ghost_width;
    return gcw;
  }



  template <int dim, int spacedim>
  void
  IFEDMethod<dim, spacedim>::reinit_interactions()
  {
    for (unsigned int part_n = 0; part_n < parts.size(); ++part_n)
      {
        const Part<dim, spacedim> &part = parts[part_n];

        const auto &tria =
          dynamic_cast<const parallel::shared::Triangulation<dim, spacedim> &>(
            part.get_triangulation());
        const DoFHandler<dim, spacedim> &dof_handler = part.get_dof_handler();
        MappingFEField<dim,
                       spacedim,
                       LinearAlgebra::distributed::Vector<double>>
                   mapping(dof_handler, part.get_position());
        const auto local_bboxes =
          compute_cell_bboxes<dim, spacedim, float>(dof_handler, mapping);

        const auto global_bboxes =
          collect_all_active_cell_bboxes(tria, local_bboxes);

        interactions[part_n]->reinit(tria,
                                     global_bboxes,
                                     secondary_hierarchy.d_secondary_hierarchy,
                                     primary_hierarchy->getFinestLevelNumber());
        // TODO - we should probably add a reinit() function that sets up the
        // DoFHandler we always need
        interactions[part_n]->add_dof_handler(part.get_dof_handler());
      }
  }


  template class IFEDMethod<NDIM - 1, NDIM>;
  template class IFEDMethod<NDIM, NDIM>;
} // namespace fdl
