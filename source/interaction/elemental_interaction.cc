#include <fiddle/base/samrai_utilities.h>

#include <fiddle/interaction/elemental_interaction.h>
#include <fiddle/interaction/interaction_utilities.h>

#include <deal.II/base/mpi.h>

#include <deal.II/fe/mapping_fe_field.h>

#include <CartesianPatchGeometry.h>

#include <cmath>
#include <numeric>

namespace fdl
{
  using namespace dealii;
  using namespace SAMRAI;

  template <int dim, int spacedim>
  ElementalInteraction<dim, spacedim>::ElementalInteraction(
    const unsigned int min_n_points_1D,
    const double       point_density,
    const DensityKind  density_kind)
    : InteractionBase<dim, spacedim>()
    , min_n_points_1D(min_n_points_1D)
    , point_density(point_density)
    , density_kind(density_kind)
  {}

  template <int dim, int spacedim>
  ElementalInteraction<dim, spacedim>::ElementalInteraction(
    const parallel::shared::Triangulation<dim, spacedim> &native_tria,
    const std::vector<BoundingBox<spacedim, float>>      &active_cell_bboxes,
    const std::vector<float>                             &active_cell_lengths,
    tbox::Pointer<hier::BasePatchHierarchy<spacedim>>     patch_hierarchy,
    const int                                             level_number,
    const unsigned int                                    min_n_points_1D,
    const double                                          point_density,
    const DensityKind                                     density_kind)
    : ElementalInteraction<dim, spacedim>(min_n_points_1D,
                                          point_density,
                                          density_kind)
  {
    reinit(native_tria,
           active_cell_bboxes,
           active_cell_lengths,
           patch_hierarchy,
           level_number);
  }

  template <int dim, int spacedim>
  void
  ElementalInteraction<dim, spacedim>::reinit(
    const parallel::shared::Triangulation<dim, spacedim> &native_tria,
    const std::vector<BoundingBox<spacedim, float>>      &active_cell_bboxes,
    const std::vector<float>                             &active_cell_lengths,
    tbox::Pointer<hier::BasePatchHierarchy<spacedim>>     patch_hierarchy,
    const int                                             level_number)
  {
    InteractionBase<dim, spacedim>::reinit(native_tria,
                                           active_cell_bboxes,
                                           active_cell_lengths,
                                           patch_hierarchy,
                                           level_number);
    // We need to implement some more quadrature families
    const auto reference_cells = native_tria.get_reference_cells();
    Assert(reference_cells.size() == 1, ExcFDLNotImplemented());
    if (!quadrature_family)
      {
        if (reference_cells.front() == ReferenceCells::get_hypercube<dim>())
          quadrature_family.reset(
            new QGaussFamily<dim>(min_n_points_1D, point_density));
        else if (reference_cells.front() == ReferenceCells::get_simplex<dim>())
          quadrature_family.reset(new QWitherdenVincentSimplexFamily<dim>(
            min_n_points_1D, point_density, density_kind));
        else
          Assert(false, ExcFDLNotImplemented());
      }

    const auto patches =
      extract_patches(patch_hierarchy->getPatchLevel(level_number));
    double patch_dx_min = std::numeric_limits<double>::max();
    if (patches.size() > 0)
      {
        const tbox::Pointer<geom::CartesianPatchGeometry<spacedim>> geometry =
          patches[0]->getPatchGeometry();
        const double *const patch_dx = geometry->getDx();
        patch_dx_min = *std::min_element(patch_dx, patch_dx + spacedim);
      }
    const double eulerian_length =
      Utilities::MPI::min(patch_dx_min, this->communicator);

    // Determine which quadrature rule we should use on each cell:
    quadrature_indices.resize(0);
    for (const auto &cell : this->overlap_tria.active_cell_iterators())
      {
        const auto   native_cell = this->overlap_tria.get_native_cell(cell);
        const double lagrangian_length =
          active_cell_lengths[native_cell->active_cell_index()];
        quadrature_indices.push_back(
          quadrature_family->get_index(eulerian_length, lagrangian_length));
      }

    // Store quadratures in a vector:
    unsigned char max_quadrature_index = 0;
    if (quadrature_indices.size() > 0)
      max_quadrature_index =
        *std::max_element(quadrature_indices.begin(), quadrature_indices.end());
    quadratures.resize(0);
    for (unsigned char i = 0; i <= max_quadrature_index; ++i)
      quadratures.push_back((*quadrature_family)[i]);
  }

  template <int dim, int spacedim>
  bool
  ElementalInteraction<dim, spacedim>::projection_is_interpolation() const
  {
    return false;
  }

  template <int dim, int spacedim>
  std::unique_ptr<TransactionBase>
  ElementalInteraction<dim, spacedim>::compute_projection_rhs_intermediate(
    std::unique_ptr<TransactionBase> t_ptr) const
  {
    auto &trans = dynamic_cast<Transaction<dim, spacedim> &>(*t_ptr);
    Assert((trans.operation ==
            Transaction<dim, spacedim>::Operation::Interpolation),
           ExcMessage("Transaction operation should be Interpolation"));
    Assert((trans.next_state ==
            Transaction<dim, spacedim>::State::Intermediate),
           ExcMessage("Transaction state should be Intermediate"));

    // Finish communication:
    trans.position_scatter.global_to_overlap_finish(*trans.native_position,
                                                    trans.overlap_position);

    MappingFEField<dim, spacedim, Vector<double>> position_mapping(
      this->get_overlap_dof_handler(*trans.native_position_dof_handler),
      trans.overlap_position);

    // Actually do the interpolation:
    compute_projection_rhs(trans.kernel_name,
                           trans.current_data_idx,
                           this->patch_map,
                           position_mapping,
                           quadrature_indices,
                           quadratures,
                           this->get_overlap_dof_handler(
                             *trans.native_dof_handler),
                           *trans.mapping,
                           trans.overlap_rhs);

    // After we compute we begin the scatter back to the native partitioning:
    trans.rhs_scatter.overlap_to_global_start(trans.overlap_rhs,
                                              trans.rhs_scatter_back_op,
                                              0,
                                              *trans.native_rhs);

    trans.next_state = Transaction<dim, spacedim>::State::Finish;

    return t_ptr;
  }

  template <int dim, int spacedim>
  std::unique_ptr<TransactionBase>
  ElementalInteraction<dim, spacedim>::compute_spread_intermediate(
    std::unique_ptr<TransactionBase> t_ptr)
  {
    auto &trans = dynamic_cast<Transaction<dim, spacedim> &>(*t_ptr);
    Assert((trans.operation ==
            Transaction<dim, spacedim>::Operation::Spreading),
           ExcMessage("Transaction operation should be Spreading"));
    Assert((trans.next_state ==
            Transaction<dim, spacedim>::State::Intermediate),
           ExcMessage("Transaction state should be Intermediate"));

    // Finish communication:
    trans.position_scatter.global_to_overlap_finish(*trans.native_position,
                                                    trans.overlap_position);

    trans.solution_scatter.global_to_overlap_finish(*trans.native_solution,
                                                    trans.overlap_solution);

    MappingFEField<dim, spacedim, Vector<double>> position_mapping(
      this->get_overlap_dof_handler(*trans.native_position_dof_handler),
      trans.overlap_position);

    // Actually do the spreading:
    compute_spread(trans.kernel_name,
                   trans.current_data_idx,
                   this->patch_map,
                   position_mapping,
                   quadrature_indices,
                   quadratures,
                   this->get_overlap_dof_handler(*trans.native_dof_handler),
                   *trans.mapping,
                   trans.overlap_solution);

    trans.next_state = Transaction<dim, spacedim>::State::Finish;

    return t_ptr;
  }

  template <int dim, int spacedim>
  std::unique_ptr<TransactionBase>
  ElementalInteraction<dim, spacedim>::add_workload_intermediate(
    std::unique_ptr<TransactionBase> t_ptr)
  {
    auto &trans = dynamic_cast<WorkloadTransaction<dim, spacedim> &>(*t_ptr);
    Assert((trans.next_state ==
            WorkloadTransaction<dim, spacedim>::State::Intermediate),
           ExcMessage("Transaction state should be Intermediate"));

    // Finish communication:
    trans.position_scatter.global_to_overlap_finish(*trans.native_position,
                                                    trans.overlap_position);

    MappingFEField<dim, spacedim, Vector<double>> position_mapping(
      this->get_overlap_dof_handler(*trans.native_position_dof_handler),
      trans.overlap_position);

    count_quadrature_points(trans.workload_index,
                            this->patch_map,
                            position_mapping,
                            quadrature_indices,
                            quadratures);

    trans.next_state = WorkloadTransaction<dim, spacedim>::State::Finish;

    return t_ptr;
  }



  template <int dim, int spacedim>
  VectorOperation::values
  ElementalInteraction<dim, spacedim>::get_rhs_scatter_type() const
  {
    return VectorOperation::add;
  }



  // instantiations
  template class ElementalInteraction<NDIM - 1, NDIM>;
  template class ElementalInteraction<NDIM, NDIM>;
} // namespace fdl
