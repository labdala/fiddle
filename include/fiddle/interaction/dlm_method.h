#ifndef included_fiddle_interaction_dlm_method_h
#define included_fiddle_interaction_dlm_method_h

#include <fiddle/base/config.h>

#include <fiddle/mechanics/force_contribution_lib.h>

#include <deal.II/base/quadrature.h>
#include <deal.II/base/subscriptor.h>

#include <deal.II/lac/la_parallel_vector.h>

namespace fdl
{
  using namespace dealii;

  /**
   * Abstract class implementing an interface for the distributed Lagrange
   * multiplier method.
   */
  template <int dim, int spacedim = dim>
  class DLMMethodBase : public Subscriptor
  {
  public:
    /**
     * Get the position of the structure (from the point of view of the
     * mechanics solver) at the specified time. This may involve interpolation
     * in time.
     */
    virtual void
    get_mechanics_position(
      const double                                time,
      LinearAlgebra::distributed::Vector<double> &position) const = 0;

    /**
     * Get a reference to the current position (from the point of view of the
     * mechanics solver), whereever it may be. Useful for initializing other
     * classes that need some position vector to make sense.
     */
    virtual const LinearAlgebra::distributed::Vector<double> &
    get_current_mechanics_position() const = 0;
  };

  /**
   * Force contribution based on a DLMMethod.
   */
  template <int dim, int spacedim = dim>
  class DLMForce : public SpringForce<dim, spacedim>
  {
  public:
    DLMForce(const Quadrature<dim> &          quad,
             const double                     spring_constant,
             const DoFHandler<dim, spacedim> &dof_handler,
             DLMMethodBase<dim, spacedim> &   dlm);

    /**
     * Set up the force at time @p time given the position and velocity of the
     * IB structure by computing a new reference position specified by the
     * DLMMethodBase pointer.
     */
    virtual void
    setup_force(
      const double                                      time,
      const LinearAlgebra::distributed::Vector<double> &position,
      const LinearAlgebra::distributed::Vector<double> &velocity) override;

  protected:
    SmartPointer<DLMMethodBase<dim, spacedim>> dlm;
  };
} // namespace fdl

#endif
