#include "actions_staeckel.h"
#include "mathutils.h"
#include <stdexcept>
#include <cmath>
#include "legacy/BaseClasses.hpp"
#include "legacy/Stackel_JS.hpp"

namespace actions{

const double ACCURACY_ACTION=1e-6;

/** parameters of potential, integrals of motion, and prolate spheroidal coordinates 
    for the AxisymmetricStaeckel action finder */
struct AxisymStaeckelParam {
    const coord::ProlSph& coordsys;
    const coord::ISimpleFunction& fncG;
    double lambda, nu;  ///< coordinates in prolate spheroidal coord.sys.
    double E;     ///< total energy
    double Lz;    ///< z-component of angular momentum
    double I3;    ///< third integral
    AxisymStaeckelParam(const coord::ProlSph& cs, const coord::ISimpleFunction& G,
        double _lambda, double _nu, double _E, double _Lz, double _I3) :
        coordsys(cs), fncG(G), lambda(_lambda), nu(_nu), E(_E), Lz(_Lz), I3(_I3) {};
};

/** auxiliary function for computing the integration limits for momentum in case Lz=0 */
static double axisymStaeckelAuxNoLz(double tauplusgamma, void* v_param)
{
    AxisymStaeckelParam* param=static_cast<AxisymStaeckelParam*>(v_param);
    double G;
    param->fncG.eval_simple(tauplusgamma-param->coordsys.gamma, &G);
    return (param->E + G) * tauplusgamma - param->I3;
}

/** auxiliary function that enters the definition of canonical momentum for 
    for the Staeckel potential: it is the numerator of eq.50 in de Zeeuw(1985);
    the argument tau is replaced by tau+gamma >= 0. */
static double axisymStaeckelAux(double tauplusgamma, void* v_param)
{
    AxisymStaeckelParam* param=static_cast<AxisymStaeckelParam*>(v_param);
    double G;
    param->fncG.eval_simple(tauplusgamma-param->coordsys.gamma, &G);
    const double tauplusalpha = tauplusgamma+param->coordsys.alpha-param->coordsys.gamma;
    return ( (param->E + G) * tauplusgamma - param->I3 ) * tauplusalpha
          - param->Lz*param->Lz/2 * tauplusgamma;
}

/** squared canonical momentum p^(tau), eq.4 in Sanders(2012);
    the argument tau is replaced by tau+gamma */
static double axisymStaeckelMomentumSq(double tauplusgamma, void* v_param)
{
    AxisymStaeckelParam* param=static_cast<AxisymStaeckelParam*>(v_param);
    const double tauplusalpha = tauplusgamma+param->coordsys.alpha-param->coordsys.gamma;
    if(tauplusalpha==0 || tauplusgamma==0) 
        return 0;   // avoid accidental infinities in the integral
    return axisymStaeckelAux(tauplusgamma, v_param) / 
        (2*tauplusalpha*tauplusalpha*tauplusgamma);
}

/** generic parameters for computing the action I = \int p(x) dx */
struct ActionIntParam {
    /// pointer to the function returning p^2(x)  (squared canonical momentum)
    double(*fncMomentumSq)(double,void*);
    void* param;       ///< additional parameters for the momentum function
    double xmin, xmax; ///< limits of integration
};

/** interface function for computing the action by integration.
    The integral \int_{xmin}^{xmax} p(x) dx is transformed into 
    \int_0^1 p(x(y)) (dx/dy) dy,  where x(y) = xmin + (xmax-xmin) y^2 (3-2y).
    The action is computed by filling the parameters ActionIntParam 
    with the pointer to function computing p^2(x) and the integration limits, 
    and calling mathutils::integrate(fncMomentum, aiparams, 0, 1).
*/
static double fncMomentum(double y, void* aiparam) {
    ActionIntParam* param=static_cast<ActionIntParam*>(aiparam);
    const double x = param->xmin + (param->xmax-param->xmin) * y*y*(3-2*y);
    const double dx = (param->xmax-param->xmin) * 6*y*(1-y);
    double val=(*(param->fncMomentumSq))(x, param->param);
    if(val<=0 || !mathutils::is_finite(val))
        return 0;
    return sqrt(val) * dx;
}

/** compute integrals of motion in the Staeckel potential of an oblate perfect ellipsoid, 
    together with the coordinates in its prolate spheroidal coordinate system */
AxisymStaeckelParam findIntegralsOfMotionOblatePerfectEllipsoid
    (const potential::StaeckelOblatePerfectEllipsoid& poten, const coord::PosVelCyl& point)
{
    double E = potential::totalEnergy(poten, point);
    double Lz= coord::Lz(point);
    const coord::ProlSph& coordsys=poten.coordsys();
    const coord::PosVelProlSph pprol = coord::toPosVel<coord::Cyl, coord::ProlSph>(point, coordsys);
    double Glambda;
    poten.eval_simple(pprol.lambda, &Glambda);
    double I3;
    if(point.z==0)   // special case: nu=0
        I3 = 0.5 * pow_2(point.vz) * (pow_2(point.R)+coordsys.gamma-coordsys.alpha);
    else   // general case: eq.3 in Sanders(2012)
        I3 = fmax(0,
            (pprol.lambda+coordsys.gamma) * 
            (E - pow_2(Lz)/2/(pprol.lambda+coordsys.alpha) + Glambda) -
            pow_2(pprol.lambdadot*(pprol.lambda-pprol.nu)) / 
            (8*(pprol.lambda+coordsys.alpha)*(pprol.lambda+coordsys.gamma)) );
    return AxisymStaeckelParam(coordsys, poten, pprol.lambda, pprol.nu, E, Lz, I3);
}

Actions ActionFinderAxisymmetricStaeckel::actions(const coord::PosVelCar& point) const
{
    // find integrals of motion, along with the prolate-spheroidal coordinates lambda,nu
    AxisymStaeckelParam data = findIntegralsOfMotionOblatePerfectEllipsoid(
        poten, coord::toPosVelCyl(point));
    if(data.E>=0)
        throw std::invalid_argument("Error in Axisymmetric Staeckel action finder: E>=0");

    Actions acts;
    acts.Jr = acts.Jz = 0;
    acts.Jphi = fabs(data.Lz);
    ActionIntParam aiparam;
    const double gamma=poten.coordsys().gamma, alpha=poten.coordsys().alpha;
    aiparam.fncMomentumSq = &axisymStaeckelMomentumSq;
    aiparam.param = &data;

    // to find the actions, we integrate p(tau) over tau in two different intervals (for Jz and for Jr);
    // to avoid roundoff errors when tau is close to -gamma we replace tau with x=tau+gamma>=0

    // precautionary measures: need to find a point x_0, close to lambda+gamma, 
    // at which p^2>0 (or equivalently a_0>0); 
    // this condition may not hold at precisely x=lambda+gamma due to numerical inaccuracies
    double x_0 = gamma+data.lambda;
    double a_0 = axisymStaeckelAux(x_0, &data);
    int num_iter_adjustment=0;
    while(a_0<=0 && num_iter_adjustment<3)
    {  // inequality could happen due to numerical roundoff, equality is a normal situation
        double der_a=mathutils::deriv(axisymStaeckelAux, &data, x_0, x_0*1e-6, 1);
        x_0-= a_0/der_a*1.1;
        x_0 = fmax(x_0, gamma-alpha);
        a_0 = axisymStaeckelAux(x_0, &data);
        num_iter_adjustment++;
    }
    double x_min_Jr=0;  // lower integration limit for Jr (0 is not yet determined)

    // J_z
    if(data.I3>0) {
        aiparam.xmin=0;
        if(data.Lz==0) {  // special case: may have either tube or box orbit in the meridional plane
            // using an auxiliary function A(tau) = (tau+gamma)(E+G(tau))-I3 such that 
            // p^2(tau)=A(tau)/(2(tau+alpha)(tau+gamma)).  Since E+G(tau)<0 and p^2(x_0)>=0, we have
            // A(-gamma)=-I3<0, A(x_0)>=0, A(infinity)<0.  Thus A(tau) must have two roots on (0,inf)
            double root = mathutils::findroot(&axisymStaeckelAuxNoLz, &data, 0, x_0);
            if(root<gamma-alpha) {  // box orbit
                aiparam.xmax=root;
                x_min_Jr=gamma-alpha;
            } else {  // tube orbit
                aiparam.xmax=gamma-alpha;
                x_min_Jr=root;
            }
        } else {  // Lz!=0, I3!=0
            aiparam.xmax = mathutils::findroot(&axisymStaeckelAux, &data, 0, gamma-alpha);
        }
        acts.Jz = mathutils::integrate(fncMomentum, &aiparam, 0, 1, ACCURACY_ACTION) * 2/M_PI;
    }

    // J_r
    if(a_0>0) {
        if(x_min_Jr==0)  // has not been determined yet
            x_min_Jr = mathutils::findroot_guess(&axisymStaeckelAux, &data, 
                gamma-alpha, x_0, (gamma-alpha+x_0)/2, true);
        aiparam.xmin = x_min_Jr;
        aiparam.xmax = mathutils::findroot_guess(&axisymStaeckelAux, &data,
            aiparam.xmin, HUGE_VAL, x_0, false);
        if(aiparam.xmin<aiparam.xmax)
            acts.Jr = mathutils::integrate(fncMomentum, &aiparam, 0, 1, ACCURACY_ACTION) / M_PI;
    }
    return acts;
}

//---------- Axisymmetric FUDGE JS --------//
#if 0
Actions ActionFinderAxisymmetricFudgeJS::actions(const coord::PosVelCar& point) const
{
    Actions_AxisymmetricStackel_Fudge aaf(poten, alpha, gamma);
    VecDoub ac=aaf.actions(toPosVelCyl(point));
    Actions acts;
    acts.Jr=ac[0];
    acts.Jz=ac[2];
    acts.Jphi=ac[1];
    return acts;
}
#else 

/** parameters of potential, integrals of motion, and prolate spheroidal coordinates 
    for the AxisymmetricStaeckel action finder */
struct AxisymStaeckelFudgeParam {
    const coord::ProlSph& coordsys;
    const potential::BasePotential& poten;
    double lambda, nu;   ///< coordinates in prolate spheroidal coord.sys.
    double E;            ///< total energy
    double Lz;           ///< z-component of angular momentum
    double Klambda, Knu; ///< approximate integrals of motion for two quasi-separable directions
    AxisymStaeckelFudgeParam(const coord::ProlSph& cs, const potential::BasePotential& _poten,
        double _lambda, double _nu, double _E, double _Lz, double _Klambda, double _Knu) :
        coordsys(cs), poten(_poten), lambda(_lambda), nu(_nu), E(_E), Lz(_Lz), Klambda(_Klambda), Knu(_Knu) {};
};
/** compute true (E, Lz) and approximate (Klambda, Knu) integrals of motion in an arbitrary 
    potential used for the Staeckel Fudge, 
    together with the coordinates in its prolate spheroidal coordinate system */
AxisymStaeckelFudgeParam findIntegralsOfMotionAxisymStaeckelFudge
    (const potential::BasePotential& poten, const coord::PosVelCyl& point, const coord::ProlSph& coordsys)
{
    double Phi;
    poten.eval(point, &Phi);
    double E=Phi+(pow_2(point.vR)+pow_2(point.vz)+pow_2(point.vphi))/2;
    double Lz= coord::Lz(point);
    const coord::PosVelProlSph pprol = coord::toPosVel<coord::Cyl, coord::ProlSph>(point, coordsys);
    double Klambda, Knu;
    Klambda = (pprol.lambda+coordsys.gamma) *
            (E - pow_2(Lz)/2/(pprol.lambda+coordsys.alpha)) -
            pow_2(pprol.lambdadot*(pprol.lambda-pprol.nu)) /
            (8*(pprol.lambda+coordsys.alpha)*(pprol.lambda+coordsys.gamma)) -
            (pprol.lambda-pprol.nu)*Phi;
    Knu =   (pprol.nu+coordsys.gamma) *
            (E - pow_2(Lz)/2/(pprol.nu+coordsys.alpha)) +
            (pprol.lambda-pprol.nu)*Phi;
    if(pprol.nu+coordsys.gamma<=0)  // z==0
        Knu += pow_2(point.vz)*(pprol.lambda-pprol.nu)/2;
    else
        Knu -= pow_2(pprol.nudot*(pprol.lambda-pprol.nu)) /
            (8*(pprol.nu+coordsys.alpha)*(pprol.nu+coordsys.gamma));
    return AxisymStaeckelFudgeParam(coordsys, poten, pprol.lambda, pprol.nu, E, Lz, Klambda, Knu);
}

static double axisymStaeckelFudgeAux(double tauplusgamma, void* v_param)
{
    AxisymStaeckelFudgeParam* param=static_cast<AxisymStaeckelFudgeParam*>(v_param);
    double lambda, nu, K, mult;
    if(tauplusgamma >= param->coordsys.gamma-param->coordsys.alpha) {  // evaluating J_lambda
        lambda=tauplusgamma-param->coordsys.gamma;
        nu=param->nu;
        mult=lambda-nu;
        K=param->Klambda;
    } else {  // evaluating J_nu
        lambda=param->lambda;
        nu=tauplusgamma-param->coordsys.gamma;
        mult=nu-lambda;
        K=param->Knu;
    }
    double Phi;
    param->poten.eval(coord::toPosCyl(coord::PosProlSph(lambda, nu, 0., param->coordsys)), &Phi);
    const double tauplusalpha = tauplusgamma+param->coordsys.alpha-param->coordsys.gamma;
    return ( param->E * tauplusalpha - pow_2(param->Lz)/2 ) * tauplusgamma
           - (K + Phi * mult) * tauplusalpha;
}

/** squared canonical momentum p^(tau), 
    the argument tau is replaced by tau+gamma */
static double axisymStaeckelFudgeMomentumSq(double tauplusgamma, void* v_param)
{
    AxisymStaeckelFudgeParam* param=static_cast<AxisymStaeckelFudgeParam*>(v_param);
    const double tauplusalpha = tauplusgamma+param->coordsys.alpha-param->coordsys.gamma;
    if(tauplusalpha==0 || tauplusgamma==0) 
        return 0;   // avoid accidental infinities in the integral
    return axisymStaeckelFudgeAux(tauplusgamma, v_param) / 
        (2*tauplusalpha*tauplusalpha*tauplusgamma);
}

Actions ActionFinderAxisymmetricFudgeJS::actions(const coord::PosVelCar& point) const
{
    const coord::ProlSph coordsys(alpha, gamma);
    // find integrals of motion, along with the prolate-spheroidal coordinates lambda,nu
    AxisymStaeckelFudgeParam data = findIntegralsOfMotionAxisymStaeckelFudge(
        poten, coord::toPosVelCyl(point), coordsys);
    if(data.E>=0)
        throw std::invalid_argument("Error in Axisymmetric Staeckel Fudge action finder: E>=0");

    Actions acts;
    acts.Jr = acts.Jz = 0;
    acts.Jphi = fabs(data.Lz);
    ActionIntParam aiparam;
    aiparam.fncMomentumSq = &axisymStaeckelFudgeMomentumSq;
    aiparam.param = &data;

    // to find the actions, we integrate p(tau) over tau in two different intervals (for Jz and for Jr);
    // to avoid roundoff errors when tau is close to -gamma we replace tau with x=tau+gamma>=0

    // precautionary measures: need to find a point x_0, close to lambda+gamma, 
    // at which p^2>0 (or equivalently a_0>0); 
    // this condition may not hold at precisely x=lambda+gamma due to numerical inaccuracies
    double x_0 = gamma+data.lambda;
    double a_0 = axisymStaeckelFudgeAux(x_0, &data);
    int num_iter_adjustment=0;
    while(a_0<=0 && num_iter_adjustment<3)
    {  // inequality could happen due to numerical roundoff, equality is a normal situation
        double der_a=mathutils::deriv(axisymStaeckelFudgeAux, &data, x_0, x_0*1e-6, 1);
        x_0-= a_0/der_a*1.1;
        x_0 = fmax(x_0, gamma-alpha);
        a_0 = axisymStaeckelAux(x_0, &data);
        num_iter_adjustment++;
    }
    double x_min_Jr=0;  // lower integration limit for Jr (0 is not yet determined)

    // J_z
    if(/*data.I3>0*/ 1) {
        aiparam.xmin=0;
#if 0
        if(data.Lz==0) {  // special case: may have either tube or box orbit in the meridional plane
            // using an auxiliary function A(tau) = (tau+gamma)(E+G(tau))-I3 such that 
            // p^2(tau)=A(tau)/(2(tau+alpha)(tau+gamma)).  Since E+G(tau)<0 and p^2(x_0)>=0, we have
            // A(-gamma)=-I3<0, A(x_0)>=0, A(infinity)<0.  Thus A(tau) must have two roots on (0,inf)
            double root = mathutils::findroot(&axisymStaeckelAuxNoLz, &data, 0, x_0);
            if(root<gamma-alpha) {  // box orbit
                aiparam.xmax=root;
                x_min_Jr=gamma-alpha;
            } else {  // tube orbit
                aiparam.xmax=gamma-alpha;
                x_min_Jr=root;
            }
        } else {  // Lz!=0, I3!=0
#endif
        aiparam.xmax = mathutils::findroot(&axisymStaeckelFudgeAux, &data, 0, gamma-alpha);
        acts.Jz = mathutils::integrate(fncMomentum, &aiparam, 0, 1, ACCURACY_ACTION) * 2/M_PI;
    }

    // J_r
    if(a_0>0) {
        if(x_min_Jr==0)  // has not been determined yet
            x_min_Jr = mathutils::findroot_guess(&axisymStaeckelFudgeAux, &data, 
                gamma-alpha, x_0, (gamma-alpha+x_0)/2, true);
        aiparam.xmin = x_min_Jr;
        aiparam.xmax = mathutils::findroot_guess(&axisymStaeckelFudgeAux, &data,
            aiparam.xmin, HUGE_VAL, x_0, false);
        if(aiparam.xmin<aiparam.xmax)
            acts.Jr = mathutils::integrate(fncMomentum, &aiparam, 0, 1, ACCURACY_ACTION) / M_PI;
    }
    return acts;
}

#endif
}  // namespace actions