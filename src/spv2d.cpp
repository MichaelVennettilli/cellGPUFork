#define ENABLE_CUDA

#include "spv2d.h"
#include "spv2d.cuh"
#include "cuda_profiler_api.h"

//simple constructor
SPV2D::SPV2D(int n, bool reprod,bool initGPURNG)
    {
    printf("Initializing %i cells with random positions in a square box...\n",n);
    Reproducible = reprod;
    Initialize(n,initGPURNG);
    setCellPreferencesUniform(1.0,4.0);
    };

//most common constructor...sets uniform cell preferences and types
SPV2D::SPV2D(int n,Dscalar A0, Dscalar P0,bool reprod,bool initGPURNG)
    {
    printf("Initializing %i cells with random positions in a square box... ",n);
    Reproducible = reprod;
    Initialize(n,initGPURNG);
    setCellPreferencesUniform(A0,P0);
    };

//take care of all class initialization functions
void SPV2D::Initialize(int n,bool initGPU)
    {
    Ncells=n;
    particleExclusions=false;
    Timestep = 0;
    triangletiming = 0.0; forcetiming = 0.0;
    setDeltaT(0.01);
    initializeDelMD(n);
    setModuliUniform(1.0,1.0);
    sortPeriod = -1;

    setv0Dr(0.05,1.0);
    forces.resize(n);
    external_forces.resize(n);
    AreaPeri.resize(n);

    cellDirectors.resize(n);
    displacements.resize(n);

    vector<int> baseEx(n,0);
    setExclusions(baseEx);
    particleExclusions=false;

    setCellDirectorsRandomly();
    cellRNGs.resize(Ncells);
    if(initGPU)
        initializeCurandStates(Ncells,1337,Timestep);
    resetLists();
    allDelSets();
    };

/*
 When sortPeriod < 0, this routine does not get called
call DelaunayMD's underlying Hilbert sort scheme, and re-index spv2d's arrays
*/
void SPV2D::spatialSorting()
    {
    spatiallySortPoints();

    //reTriangulate with the new ordering
    globalTriangulationCGAL();
    //get new DelSets and DelOthers
    resetLists();
    allDelSets();

    //re-index all cell information arrays
    //motility
    reIndexArray(Motility);

    //moduli
    reIndexArray(Moduli);

    //preference
    reIndexArray(AreaPeriPreferences);

    //director
    reIndexArray(cellDirectors);

    //exclusions
    reIndexArray(exclusions);

    //cellType
    reIndexArray(CellType);
    };

/*!
As the code is modified, all GPUArrays whose size depend on neighMax should be added to this function
*/
void SPV2D::resetLists()
    {
    voroCur.resize(neighMax*Ncells);
    voroLastNext.resize(neighMax*Ncells);
    delSets.resize(neighMax*Ncells);
    delOther.resize(neighMax*Ncells);
    forceSets.resize(neighMax*Ncells);
    };

//DelSets is a helper data structure keeping track of some ordering of the Delaunay vertices around a given vertex. This updates it
void SPV2D::allDelSets()
    {
    updateNeighIdxs();
    for (int ii = 0; ii < Ncells; ++ii)
        getDelSets(ii);
    };

//update the delSet and delOther structure just for a particular particle
bool SPV2D::getDelSets(int i)
    {
    ArrayHandle<int> neighnum(cellNeighborNum,access_location::host,access_mode::read);
    ArrayHandle<int> ns(cellNeighbors,access_location::host,access_mode::read);
    ArrayHandle<int2> ds(delSets,access_location::host,access_mode::readwrite);
    ArrayHandle<int> dother(delOther,access_location::host,access_mode::readwrite);

    int iNeighs = neighnum.data[i];
    int nm2,nm1,n1,n2;
    nm2 = ns.data[n_idx(iNeighs-3,i)];
    nm1 = ns.data[n_idx(iNeighs-2,i)];
    n1 = ns.data[n_idx(iNeighs-1,i)];

    for (int nn = 0; nn < iNeighs; ++nn)
        {
        n2 = ns.data[n_idx(nn,i)];
        int nextNeighs = neighnum.data[n1];
        for (int nn2 = 0; nn2 < nextNeighs; ++nn2)
            {
            int testPoint = ns.data[n_idx(nn2,n1)];
            if(testPoint == nm1)
                {
                dother.data[n_idx(nn,i)] = ns.data[n_idx((nn2+1)%nextNeighs,n1)];
                break;
                };
            };
        ds.data[n_idx(nn,i)].x= nm1;
        ds.data[n_idx(nn,i)].y= n1;

        //is "delOther" a copy of i or either of the delSet points? if so, the local topology is inconsistent
        if(nm1 == dother.data[n_idx(nn,i)] || n1 == dother.data[n_idx(nn,i)] || i == dother.data[n_idx(nn,i)])
            return false;

        nm2=nm1;
        nm1=n1;
        n1=n2;

        };
    return true;
    };


/*!
\param exes a list of per-particle indications of whether a particle should be excluded (exes[i] !=0) or not/
*/
void SPV2D::setExclusions(vector<int> &exes)
    {
    particleExclusions=true;
    external_forces.resize(Ncells);
    exclusions.resize(Ncells);
    ArrayHandle<Dscalar2> h_mot(Motility,access_location::host,access_mode::readwrite);
    ArrayHandle<int> h_ex(exclusions,access_location::host,access_mode::overwrite);

    for (int ii = 0; ii < Ncells; ++ii)
        {
        h_ex.data[ii] = 0;
        if( exes[ii] != 0)
            {
            //set v0 to zero and Dr to zero
            h_mot.data[ii].x = 0.0;
            h_mot.data[ii].y = 0.0;
            h_ex.data[ii] = 1;
            };
        };
    };

/*!
Call all relevant functions to advance the system one time step; every sortPeriod also call the
spatial sorting routine.
*/
void SPV2D::performTimestep()
    {
    Timestep += 1;

    spatialSortThisStep = false;
    if (sortPeriod > 0)
        {
        if (Timestep % sortPeriod == 0)
            {
            spatialSortThisStep = true;
            };
        };

    if(GPUcompute)
        performTimestepGPU();
    else
        performTimestepCPU();

    if (spatialSortThisStep)
        spatialSorting();
    };

/*!
if forces have already been computed, displace particles according to net force and motility,
and rotate the cell directors via a cuda call
*/
void SPV2D::DisplacePointsAndRotate()
    {

    ArrayHandle<Dscalar2> d_p(cellPositions,access_location::device,access_mode::readwrite);
    ArrayHandle<Dscalar2> d_f(forces,access_location::device,access_mode::read);
    ArrayHandle<Dscalar> d_cd(cellDirectors,access_location::device,access_mode::readwrite);
    ArrayHandle<Dscalar2> d_motility(Motility,access_location::device,access_mode::read);
    ArrayHandle<curandState> d_cs(cellRNGs,access_location::device,access_mode::read);

    gpu_displace_and_rotate(d_p.data,
                            d_f.data,
                            d_cd.data,
                            d_motility.data,
                            Ncells,
                            deltaT,
                            Timestep,
                            d_cs.data,
                            Box);

    };

/*!
if forces have already been computed, displace particles according to net force and motility,
and rotate the cell directors via the CPU
*/
void SPV2D::calculateDispCPU()
    {
    ArrayHandle<Dscalar2> h_f(forces,access_location::host,access_mode::read);
    ArrayHandle<Dscalar> h_cd(cellDirectors,access_location::host,access_mode::readwrite);
    ArrayHandle<Dscalar2> h_disp(displacements,access_location::host,access_mode::overwrite);
    ArrayHandle<Dscalar2> h_motility(Motility,access_location::host,access_mode::read);

    random_device rd;
    mt19937 gen(rand());
    normal_distribution<> normal(0.0,1.0);
    for (int ii = 0; ii < Ncells; ++ii)
        {
        Dscalar v0i = h_motility.data[ii].x;
        Dscalar Dri = h_motility.data[ii].y;
        Dscalar dx,dy;
        Dscalar directorx = cos(h_cd.data[ii]);
        Dscalar directory = sin(h_cd.data[ii]);

        dx= deltaT*(v0*directorx+h_f.data[ii].x);
        dy= deltaT*(v0*directory+h_f.data[ii].y);
        h_disp.data[ii].x = dx;
        h_disp.data[ii].y = dy;

        //rotate each director a bit
        h_cd.data[ii] +=normal(gen)*sqrt(2.0*deltaT*Dri);
        };
    //vector of displacements is forces*timestep + v0's*timestep
    };

//perform a timestep on the CPU
void SPV2D::performTimestepCPU()
    {
    computeGeometryCPU();
    for (int ii = 0; ii < Ncells; ++ii)
        computeSPVForceCPU(ii);
    calculateDispCPU();

    movePointsCPU(displacements);
    if(!spatialSortThisStep)
        {
        testAndRepairTriangulation();
        if(neighMaxChange)
            {
            if(neighMaxChange)
                {
                resetLists();
                };
            neighMaxChange = false;
            allDelSets();
            };
        };
    };

/*!
If the geoemtry has already been calculated, call the right function to calculate the
contribution to the net force on every particle from each of its voronoi vertices via
a cuda call
*/
void SPV2D::ComputeForceSetsGPU()
    {
        computeSPVForceSetsGPU();
    };

/*!
If the force_sets are already computed, call the right function to add them up to get the
net force per particle via a cuda call
*/
void SPV2D::SumForcesGPU()
    {
    if(!particleExclusions)
        sumForceSets();
    else
        sumForceSetsWithExclusions();
    };

//perform a timestep on the GPU
void SPV2D::performTimestepGPU()
    {
    computeGeometryGPU();
    ComputeForceSetsGPU();
    SumForcesGPU();
    DisplacePointsAndRotate();

    //spatial sorting triggers a global re-triangulation, so no need to test and repair
    //
    if(!spatialSortThisStep)
        {
        testAndRepairTriangulation();

        if(anyCircumcenterTestFailed == 1)
            {
            //maintain the auxilliary lists for computing forces
            if(completeRetriangulationPerformed || neighMaxChange)
                {
                if(neighMaxChange)
                    {
                    resetLists();
                    neighMaxChange = false;
                    };
                allDelSets();
                }
            else
                {
                bool localFail = false;
                for (int jj = 0;jj < NeedsFixing.size(); ++jj)
                    {
                    if(!getDelSets(NeedsFixing[jj]))
                        localFail=true;
                    };
                if (localFail)
                    {
                    cout << "Local triangulation failed to return a consistent set of topological information..." << endl;
                    cout << "Now attempting a global re-triangulation to save the day." << endl;
                    globalTriangulationCGAL();
                    //get new DelSets and DelOthers
                    resetLists();
                    allDelSets();
                    };
                };

            };

        //pre-copy some data back to device; this will overlap with some CPU time
        //...these are the arrays that are used by force_sets but not geometry, and should be switched to Async
        ArrayHandle<int2> d_delSets(delSets,access_location::device,access_mode::read);
        ArrayHandle<int> d_delOther(delOther,access_location::device,access_mode::read);
        ArrayHandle<int2> d_nidx(NeighIdxs,access_location::device,access_mode::read);

        };
    };

/*!
If the topology is up-to-date on the GPU, calculate all cell areas, perimenters, and voronoi neighbors
*/
void SPV2D::computeGeometryGPU()
    {
    ArrayHandle<Dscalar2> d_p(cellPositions,access_location::device,access_mode::read);
    ArrayHandle<Dscalar2> d_AP(AreaPeri,access_location::device,access_mode::readwrite);
    ArrayHandle<int> d_nn(cellNeighborNum,access_location::device,access_mode::read);
    ArrayHandle<int> d_n(cellNeighbors,access_location::device,access_mode::read);
    ArrayHandle<Dscalar2> d_vc(voroCur,access_location::device,access_mode::overwrite);
    ArrayHandle<Dscalar4> d_vln(voroLastNext,access_location::device,access_mode::overwrite);

    gpu_compute_geometry(
                        d_p.data,
                        d_AP.data,
                        d_nn.data,
                        d_n.data,
                        d_vc.data,
                        d_vln.data,
                        Ncells, n_idx,Box);
    };

/*!
If the force_sets are already computed, add them up to get the net force per particle
via a cuda call
*/
void SPV2D::sumForceSets()
    {

    ArrayHandle<int> d_nn(cellNeighborNum,access_location::device,access_mode::read);
    ArrayHandle<Dscalar2> d_forceSets(forceSets,access_location::device,access_mode::read);
    ArrayHandle<Dscalar2> d_forces(forces,access_location::device,access_mode::overwrite);

    gpu_sum_force_sets(
                    d_forceSets.data,
                    d_forces.data,
                    d_nn.data,
                    Ncells,n_idx);
    };

/*!
If the force_sets are already computed, add them up to get the net force per particle
via a cuda call, assuming some particle exclusions have been defined
*/
void SPV2D::sumForceSetsWithExclusions()
    {

    ArrayHandle<int> d_nn(cellNeighborNum,access_location::device,access_mode::read);
    ArrayHandle<Dscalar2> d_forceSets(forceSets,access_location::device,access_mode::read);
    ArrayHandle<Dscalar2> d_forces(forces,access_location::device,access_mode::overwrite);
    ArrayHandle<Dscalar2> d_external_forces(external_forces,access_location::device,access_mode::overwrite);
    ArrayHandle<int> d_exes(exclusions,access_location::device,access_mode::read);

    gpu_sum_force_sets_with_exclusions(
                    d_forceSets.data,
                    d_forces.data,
                    d_external_forces.data,
                    d_exes.data,
                    d_nn.data,
                    Ncells,n_idx);
    };


/*!
Calculate the contributions to the net force on particle "i" from each of particle i's voronoi
vertices
*/
void SPV2D::computeSPVForceSetsGPU()
    {
    ArrayHandle<Dscalar2> d_p(cellPositions,access_location::device,access_mode::read);
    ArrayHandle<Dscalar2> d_AP(AreaPeri,access_location::device,access_mode::read);
    ArrayHandle<Dscalar2> d_APpref(AreaPeriPreferences,access_location::device,access_mode::read);
    ArrayHandle<int2> d_delSets(delSets,access_location::device,access_mode::read);
    ArrayHandle<int> d_delOther(delOther,access_location::device,access_mode::read);
    ArrayHandle<Dscalar2> d_forceSets(forceSets,access_location::device,access_mode::overwrite);
    ArrayHandle<int2> d_nidx(NeighIdxs,access_location::device,access_mode::read);
    ArrayHandle<Dscalar2> d_vc(voroCur,access_location::device,access_mode::read);
    ArrayHandle<Dscalar4> d_vln(voroLastNext,access_location::device,access_mode::read);

    Dscalar KA = 1.0;
    Dscalar KP = 1.0;
    gpu_force_sets(
                    d_p.data,
                    d_AP.data,
                    d_APpref.data,
                    d_delSets.data,
                    d_delOther.data,
                    d_vc.data,
                    d_vln.data,
                    d_forceSets.data,
                    d_nidx.data,
                    KA,
                    KP,
                    NeighIdxNum,n_idx,Box);
    };

//compute cell area and perimeter on the CPU
void SPV2D::computeGeometryCPU()
    {
    //read in all the data we'll need
    ArrayHandle<Dscalar2> h_p(cellPositions,access_location::host,access_mode::read);
    ArrayHandle<Dscalar2> h_AP(AreaPeri,access_location::host,access_mode::readwrite);
    ArrayHandle<int> h_nn(cellNeighborNum,access_location::host,access_mode::read);
    ArrayHandle<int> h_n(cellNeighbors,access_location::host,access_mode::read);

    ArrayHandle<Dscalar2> h_v(voroCur,access_location::host,access_mode::overwrite);

    for (int i = 0; i < Ncells; ++i)
        {
        //get Delaunay neighbors of the cell
        int neigh = h_nn.data[i];
        vector<int> ns(neigh);
        for (int nn = 0; nn < neigh; ++nn)
            {
            ns[nn]=h_n.data[n_idx(nn,i)];
            };

        //compute base set of voronoi points, and the derivatives of those points w/r/t cell i's position
        vector<Dscalar2> voro(neigh);
        Dscalar2 circumcent;
        Dscalar2 nnextp,nlastp;
        Dscalar2 pi = h_p.data[i];
        Dscalar2 rij, rik;

        nlastp = h_p.data[ns[ns.size()-1]];
        Box.minDist(nlastp,pi,rij);
        for (int nn = 0; nn < neigh;++nn)
            {
            nnextp = h_p.data[ns[nn]];
            Box.minDist(nnextp,pi,rik);
            Circumcenter(rij,rik,circumcent);
            voro[nn] = circumcent;
            rij=rik;
            int id = n_idx(nn,i);
            h_v.data[id] = voro[nn];
            };

        Dscalar2 vlast,vnext;
        //compute Area and perimeter
        Dscalar Varea = 0.0;
        Dscalar Vperi = 0.0;
        vlast = voro[neigh-1];
        for (int nn = 0; nn < neigh; ++nn)
            {
            vnext=voro[nn];
            Varea += TriangleArea(vlast,vnext);
            Dscalar dx = vlast.x-vnext.x;
            Dscalar dy = vlast.y-vnext.y;
            Vperi += sqrt(dx*dx+dy*dy);
            vlast=vnext;
            };
        h_AP.data[i].x = Varea;
        h_AP.data[i].y = Vperi;
        };
    };

/*!
\param i The particle index for which to compute the net force, assuming addition tension terms between unlike particles
*/
void SPV2D::computeSPVForceCPU(int i)
    {
    Dscalar Pthreshold = THRESHOLD;

    //read in all the data we'll need
    ArrayHandle<Dscalar2> h_p(cellPositions,access_location::host,access_mode::read);
    ArrayHandle<Dscalar2> h_f(forces,access_location::host,access_mode::readwrite);
    ArrayHandle<int> h_ct(CellType,access_location::host,access_mode::read);
    ArrayHandle<Dscalar2> h_AP(AreaPeri,access_location::host,access_mode::read);
    ArrayHandle<Dscalar2> h_APpref(AreaPeriPreferences,access_location::host,access_mode::read);
    ArrayHandle<Dscalar2> h_v(voroCur,access_location::host,access_mode::read);

    ArrayHandle<int> h_nn(cellNeighborNum,access_location::host,access_mode::read);
    ArrayHandle<int> h_n(cellNeighbors,access_location::host,access_mode::read);

    ArrayHandle<Dscalar2> h_external_forces(external_forces,access_location::host,access_mode::overwrite);
    ArrayHandle<int> h_exes(exclusions,access_location::host,access_mode::read);


    //get Delaunay neighbors of the cell
    int neigh = h_nn.data[i];
    vector<int> ns(neigh);
    for (int nn = 0; nn < neigh; ++nn)
        {
        ns[nn]=h_n.data[n_idx(nn,i)];
        };

    //compute base set of voronoi points, and the derivatives of those points w/r/t cell i's position
    vector<Dscalar2> voro(neigh);
    vector<Matrix2x2> dhdri(neigh);
    Matrix2x2 Id;
    Dscalar2 circumcent;
    Dscalar2 rij,rik;
    Dscalar2 nnextp,nlastp;
    Dscalar2 rjk;
    Dscalar2 pi = h_p.data[i];

    nlastp = h_p.data[ns[ns.size()-1]];
    Box.minDist(nlastp,pi,rij);
    for (int nn = 0; nn < neigh;++nn)
        {
        int id = n_idx(nn,i);
        nnextp = h_p.data[ns[nn]];
        Box.minDist(nnextp,pi,rik);
        voro[nn] = h_v.data[id];
        rjk.x =rik.x-rij.x;
        rjk.y =rik.y-rij.y;

        Dscalar2 dbDdri,dgDdri,dDdriOD,z;
        Dscalar betaD = -dot(rik,rik)*dot(rij,rjk);
        Dscalar gammaD = dot(rij,rij)*dot(rik,rjk);
        Dscalar cp = rij.x*rjk.y - rij.y*rjk.x;
        Dscalar D = 2*cp*cp;


        z.x = betaD*rij.x+gammaD*rik.x;
        z.y = betaD*rij.y+gammaD*rik.y;

        dbDdri.x = 2*dot(rij,rjk)*rik.x+dot(rik,rik)*rjk.x;
        dbDdri.y = 2*dot(rij,rjk)*rik.y+dot(rik,rik)*rjk.y;

        dgDdri.x = -2*dot(rik,rjk)*rij.x-dot(rij,rij)*rjk.x;
        dgDdri.y = -2*dot(rik,rjk)*rij.y-dot(rij,rij)*rjk.y;

        dDdriOD.x = (-2.0*rjk.y)/cp;
        dDdriOD.y = (2.0*rjk.x)/cp;

        dhdri[nn] = Id+1.0/D*(dyad(rij,dbDdri)+dyad(rik,dgDdri)-(betaD+gammaD)*Id-dyad(z,dDdriOD));

        rij=rik;
        };

    Dscalar2 vlast,vnext,vother;
    vlast = voro[neigh-1];

    //start calculating forces
    Dscalar2 forceSum;
    forceSum.x=0.0;forceSum.y=0.0;
    Dscalar KA = 1.0;
    Dscalar KP = 1.0;

    Dscalar Adiff = KA*(h_AP.data[i].x - h_APpref.data[i].x);
    Dscalar Pdiff = KP*(h_AP.data[i].y - h_APpref.data[i].y);

    Dscalar2 vcur;
    vlast = voro[neigh-1];
    for(int nn = 0; nn < neigh; ++nn)
        {
        //first, let's do the self-term, dE_i/dr_i
        vcur = voro[nn];
        vnext = voro[(nn+1)%neigh];
        int baseNeigh = ns[nn];
        int other_idx = nn - 1;
        if (other_idx < 0) other_idx += neigh;
        int otherNeigh = ns[other_idx];


        Dscalar2 dAidv,dPidv;
        dAidv.x = 0.5*(vlast.y-vnext.y);
        dAidv.y = 0.5*(vnext.x-vlast.x);

        Dscalar2 dlast,dnext;
        dlast.x = vlast.x-vcur.x;
        dlast.y=vlast.y-vcur.y;

        Dscalar dlnorm = sqrt(dlast.x*dlast.x+dlast.y*dlast.y);

        dnext.x = vcur.x-vnext.x;
        dnext.y = vcur.y-vnext.y;
        Dscalar dnnorm = sqrt(dnext.x*dnext.x+dnext.y*dnext.y);
        if(dnnorm < Pthreshold)
            dnnorm = Pthreshold;
        if(dlnorm < Pthreshold)
            dlnorm = Pthreshold;
        dPidv.x = dlast.x/dlnorm - dnext.x/dnnorm;
        dPidv.y = dlast.y/dlnorm - dnext.y/dnnorm;

        //
        //now let's compute the other terms...first we need to find the third voronoi
        //position that v_cur is connected to
        //
        int neigh2 = h_nn.data[baseNeigh];
        int DT_other_idx=-1;
        for (int n2 = 0; n2 < neigh2; ++n2)
            {
            int testPoint = h_n.data[n_idx(n2,baseNeigh)];
            if(testPoint == otherNeigh) DT_other_idx = h_n.data[n_idx((n2+1)%neigh2,baseNeigh)];
            };
        if(DT_other_idx == otherNeigh || DT_other_idx == baseNeigh || DT_other_idx == -1)
            {
            printf("Triangulation problem %i\n",DT_other_idx);
            throw std::exception();
            };
        Dscalar2 nl1 = h_p.data[otherNeigh];
        Dscalar2 nn1 = h_p.data[baseNeigh];
        Dscalar2 no1 = h_p.data[DT_other_idx];

        Dscalar2 r1,r2,r3;
        Box.minDist(nl1,pi,r1);
        Box.minDist(nn1,pi,r2);
        Box.minDist(no1,pi,r3);

        Circumcenter(r1,r2,r3,vother);

        Dscalar Akdiff = KA*(h_AP.data[baseNeigh].x  - h_APpref.data[baseNeigh].x);
        Dscalar Pkdiff = KP*(h_AP.data[baseNeigh].y  - h_APpref.data[baseNeigh].y);
        Dscalar Ajdiff = KA*(h_AP.data[otherNeigh].x - h_APpref.data[otherNeigh].x);
        Dscalar Pjdiff = KP*(h_AP.data[otherNeigh].y - h_APpref.data[otherNeigh].y);

        Dscalar2 dAkdv,dPkdv;
        dAkdv.x = 0.5*(vnext.y-vother.y);
        dAkdv.y = 0.5*(vother.x-vnext.x);

        dlast.x = vnext.x-vcur.x;
        dlast.y=vnext.y-vcur.y;
        dlnorm = sqrt(dlast.x*dlast.x+dlast.y*dlast.y);
        dnext.x = vcur.x-vother.x;
        dnext.y = vcur.y-vother.y;
        dnnorm = sqrt(dnext.x*dnext.x+dnext.y*dnext.y);
        if(dnnorm < Pthreshold)
            dnnorm = Pthreshold;
        if(dlnorm < Pthreshold)
            dlnorm = Pthreshold;

        dPkdv.x = dlast.x/dlnorm - dnext.x/dnnorm;
        dPkdv.y = dlast.y/dlnorm - dnext.y/dnnorm;

        Dscalar2 dAjdv,dPjdv;
        dAjdv.x = 0.5*(vother.y-vlast.y);
        dAjdv.y = 0.5*(vlast.x-vother.x);

        dlast.x = vother.x-vcur.x;
        dlast.y=vother.y-vcur.y;
        dlnorm = sqrt(dlast.x*dlast.x+dlast.y*dlast.y);
        dnext.x = vcur.x-vlast.x;
        dnext.y = vcur.y-vlast.y;
        dnnorm = sqrt(dnext.x*dnext.x+dnext.y*dnext.y);
        if(dnnorm < Pthreshold)
            dnnorm = Pthreshold;
        if(dlnorm < Pthreshold)
            dlnorm = Pthreshold;

        dPjdv.x = dlast.x/dlnorm - dnext.x/dnnorm;
        dPjdv.y = dlast.y/dlnorm - dnext.y/dnnorm;

        Dscalar2 dEdv;

        dEdv.x = 2.0*Adiff*dAidv.x + 2.0*Pdiff*dPidv.x;
        dEdv.y = 2.0*Adiff*dAidv.y + 2.0*Pdiff*dPidv.y;
        dEdv.x += 2.0*Akdiff*dAkdv.x + 2.0*Pkdiff*dPkdv.x;
        dEdv.y += 2.0*Akdiff*dAkdv.y + 2.0*Pkdiff*dPkdv.y;
        dEdv.x += 2.0*Ajdiff*dAjdv.x + 2.0*Pjdiff*dPjdv.x;
        dEdv.y += 2.0*Ajdiff*dAjdv.y + 2.0*Pjdiff*dPjdv.y;

        Dscalar2 temp = dEdv*dhdri[nn];
        forceSum.x += temp.x;
        forceSum.y += temp.y;

        vlast=vcur;
        };

    h_f.data[i].x=forceSum.x;
    h_f.data[i].y=forceSum.y;
    if(particleExclusions)
        {
        if(h_exes.data[i] != 0)
            {
            h_f.data[i].x = 0.0;
            h_f.data[i].y = 0.0;
            h_external_forces.data[i].x=-forceSum.x;
            h_external_forces.data[i].y=-forceSum.y;
            };
        }
    };

//a utility testing function...calculate the average area of the cells
void SPV2D::meanArea()
    {
    ArrayHandle<Dscalar2> h_AP(AreaPeri,access_location::host,access_mode::read);
    Dscalar fx = 0.0;
    for (int i = 0; i < Ncells; ++i)
        {
        fx += h_AP.data[i].x/Ncells;
        };
    printf("Mean area = %f\n" ,fx);
    };

//a utility/testing function...output the currently computed forces to the screen
void SPV2D::reportForces()
    {
    ArrayHandle<Dscalar2> h_f(forces,access_location::host,access_mode::read);
    ArrayHandle<Dscalar2> p(cellPositions,access_location::host,access_mode::read);
    Dscalar fx = 0.0;
    Dscalar fy = 0.0;
    Dscalar min = 10000;
    Dscalar max = -10000;
    for (int i = 0; i < Ncells; ++i)
        {
        if (h_f.data[i].y >max)
            max = h_f.data[i].y;
        if (h_f.data[i].x >max)
            max = h_f.data[i].x;
        if (h_f.data[i].y < min)
            min = h_f.data[i].y;
        if (h_f.data[i].x < min)
            min = h_f.data[i].x;
        fx += h_f.data[i].x;
        fy += h_f.data[i].y;

        printf("cell %i: \t position (%f,%f)\t force (%e, %e)\n",i,p.data[i].x,p.data[i].y ,h_f.data[i].x,h_f.data[i].y);
        };
    printf("min/max force : (%f,%f)\n",min,max);

    };

//a utility/testing function...report the sum (not the mean!) of all net forces on all particles. It had better be close to zero.
void SPV2D::meanForce()
    {
    ArrayHandle<Dscalar2> h_f(forces,access_location::host,access_mode::read);
    Dscalar fx = 0.0;
    Dscalar fy = 0.0;
    for (int i = 0; i < Ncells; ++i)
        {
        fx += h_f.data[i].x;
        fy += h_f.data[i].y;
        };
    printf("Mean force = (%e,%e)\n" ,fx/Ncells,fy/Ncells);
    };

//a utility function...output some information assuming the system is uniform
void SPV2D::reportCellInfo()
    {
    printf("Ncells=%i\tv0=%f\tDr=%f\n",Ncells,v0,Dr);
    };

