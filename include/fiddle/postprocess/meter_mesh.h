#ifndef included_fiddle_postprocess_meter_mesh_h
#define included_fiddle_postprocess_meter_mesh_h

#include <fiddle/base/config.h>

#include <fiddle/interaction/nodal_interaction.h>

#include <deal.II/base/point.h>
#include <deal.II/base/smartpointer.h>
#include <deal.II/base/tensor.h>

#include <deal.II/dofs/dof_handler.h>

#include <deal.II/fe/mapping.h>

#include <deal.II/lac/la_parallel_vector.h>

#include <fiddle/postprocess/point_values.h>

#include <memory>
#include <vector>

namespace fdl
{
  using namespace dealii;

  /**
   * Class for integrating Cartesian-grid values on codimension one surfaces
   * (colloquially a 'meter mesh').
   */
  template <int dim, int spacedim = dim>
  class MeterMesh
  {
  public:
    /**
     * Constructor.
     *
     * @param[in] mapping Mapping defined in reference coordinates (e.g., the
     * mapping returned by Part::get_mapping())
     *
     * @param[in] position_dof_handler DoFHandler describing the position and
     * velocity finite element spaces.
     *
     * @param[in] convex_hull Points, in reference coordinates, describing the
     * boundary of the meter mesh. These points typically outline a disk and
     * typically come from a node set defined on the Triangulation associated
     * with @p dof_handler.
     */
    MeterMesh(const Mapping<dim, spacedim>       &mapping,
              const DoFHandler<dim, spacedim>    &position_dof_handler,
              const std::vector<Point<spacedim>> &convex_hull,
              tbox::Pointer<hier::BasePatchHierarchy<spacedim>> patch_hierarchy,
              const int                                         level_number,
              const LinearAlgebra::distributed::Vector<double> &position);

    /**
     * Reinitialize the meter mesh to have its coordinates specified by @p
     * position and velocity by @p velocity.
     */
    void
    reinit(const LinearAlgebra::distributed::Vector<double> &position);

    /**
     * Return a reference to the meter Triangulation. This triangulation is
     * not in reference coordinates: instead its absolute position is
     * determined by the position of the original volumetric structure.
     */
    const Triangulation<dim - 1, spacedim> &
    get_meter_tria() const;

    /**
     * Return the mean meter velocity, defined as the average value of the
     * velocity field specified by @p data_idx on the boundary of the meter
     * mesh.
     */
    Tensor<1, spacedim>
    mean_meter_velocity(const int data_idx, const std::string &kernel_name);

    /**
     * Compute the mean flux of some vector-valued quantity through the meter
     * mesh. If @p data_idx is the velocity field then typically one should
     * subtract the mean meter velocity from this value to attain a physically
     * relevant flux value.
     *
     * @param[in] data_idx Some data index corresponding to data on the
     * Cartesian grid. This class will copy the data into a scratch index and
     * update ghost data.
     */
    Tensor<1, spacedim>
    mean_flux(const int data_idx, const std::string &kernel_name);

    /**
     * Compute the mean value of some scalar-valued quantity.
     *
     * @param[in] data_idx Some data index corresponding to data on the
     * Cartesian grid. This class will copy the data into a scratch index and
     * update ghost data.
     */
    double
    mean_value(const int data_idx, const std::string &kernel_name);

  protected:
    /**
     * Original Mapping.
     */
    SmartPointer<const Mapping<dim, spacedim>> mapping;

    /**
     * Cartesian-grid data.
     * @{
     */
    tbox::Pointer<hier::BasePatchHierarchy<spacedim>> patch_hierarchy;

    int level_number;
    /**
     * @}
     */

    /**
     * PointValues object for computing the mesh's position.
     */
    std::unique_ptr<PointValues<spacedim, dim, spacedim>> point_values;

    /**
     * Meter Triangulation.
     */
    parallel::shared::Triangulation<dim - 1, spacedim> meter_tria;

    /**
     * Scalar FiniteElement used on meter_tria
     */
    std::unique_ptr<FiniteElement<dim - 1, spacedim>> scalar_fe;

    /**
     * Vector FiniteElement used on meter_tria
     */
    std::unique_ptr<FiniteElement<dim - 1, spacedim>> vector_fe;

    /**
     * DoFHandler for scalar quantities defined on meter_tria.
     */
    DoFHandler<dim - 1, spacedim> scalar_dof_handler;

    /**
     * DoFHandler for vector-valued quantities defined on meter_tria.
     */
    DoFHandler<dim - 1, spacedim> vector_dof_handler;


    std::shared_ptr<Utilities::MPI::Partitioner> vector_partitioner;

    std::shared_ptr<Utilities::MPI::Partitioner> scalar_partitioner;

    /**
     * Interaction object.
     */
    std::unique_ptr<NodalInteraction<dim - 1, spacedim>> nodal_interaction;
  };

  template <int dim, int spacedim>
  inline const Triangulation<dim - 1, spacedim> &
  MeterMesh<dim, spacedim>::get_meter_tria() const
  {
      return meter_tria;
  }
} // namespace fdl

#endif
