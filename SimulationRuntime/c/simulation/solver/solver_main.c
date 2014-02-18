/*
 * This file is part of OpenModelica.
 *
 * Copyright (c) 1998-CurrentYear, Linköping University,
 * Department of Computer and Information Science,
 * SE-58183 Linköping, Sweden.
 *
 * All rights reserved.
 *
 * THIS PROGRAM IS PROVIDED UNDER THE TERMS OF THIS OSMC PUBLIC
 * LICENSE (OSMC-PL). ANY USE, REPRODUCTION OR DISTRIBUTION OF
 * THIS PROGRAM CONSTITUTES RECIPIENT'S ACCEPTANCE OF THE OSMC
 * PUBLIC LICENSE.
 *
 * The OpenModelica software and the Open Source Modelica
 * Consortium (OSMC) Public License (OSMC-PL) are obtained
 * from Linköping University, either from the above address,
 * from the URL: http://www.ida.liu.se/projects/OpenModelica
 * and in the OpenModelica distribution.
 *
 * This program is distributed  WITHOUT ANY WARRANTY; without
 * even the implied warranty of  MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE, EXCEPT AS EXPRESSLY SET FORTH
 * IN THE BY RECIPIENT SELECTED SUBSIDIARY LICENSE CONDITIONS
 * OF OSMC-PL.
 *
 * See the full OSMC Public License conditions for more details.
 *
 */

 /*! \file solver_main.c
 */

#include "../../Compiler/runtime/config.h"
#include "solver_main.h"
#include "simulation_runtime.h"
#include "simulation_result.h"
#include "openmodelica_func.h"
#include "initialization.h"
#include "nonlinearSystem.h"
#include "dassl.h"
#include "delay.h"
#include "events.h"
#include "varinfo.h"
#include "stateset.h"
#include "radau.h"
#include "model_help.h"

#include "interfaceOptimization.h"
#include "simulation_inline_solver.h"

/*
 * #include "dopri45.h"
 */
#include "rtclock.h"
#include "omc_error.h"
#include "options.h"
#include <math.h>
#include <string.h>
#include <errno.h>
#include <float.h>

double** work_states;

const int rungekutta_s = 4;
const double rungekutta_b[4] = { 1.0 / 6.0, 1.0 / 3.0, 1.0 / 3.0, 1.0 / 6.0 };
const double rungekutta_c[4] = { 0.0, 0.5, 0.5, 1.0 };

typedef struct RK4_DATA
{
  double** work_states;
  int work_states_ndims;
}RK4_DATA;


static int euler_ex_step(DATA* data, SOLVER_INFO* solverInfo);
static int rungekutta_step(DATA* data, SOLVER_INFO* solverInfo);

static int radau_lobatto_step(DATA* data, SOLVER_INFO* solverInfo);

#ifdef WITH_IPOPT
static int ipopt_step(DATA* data, SOLVER_INFO* solverInfo);
#endif

static void writeOutputVars(char* names, DATA* data);

int solver_main_step(DATA* data, SOLVER_INFO* solverInfo)
{
  switch(solverInfo->solverMethod)
  {
  case 2:
    return rungekutta_step(data, solverInfo);

  case 3:
    return dasrt_step(data, solverInfo);

  case 4:
    data->callback->functionODE_inline(data, solverInfo->currentStepSize);
    solverInfo->currentTime = data->localData[0]->timeValue;
    return 0;
#ifdef WITH_IPOPT
  case 5:
    return ipopt_step(data, solverInfo);
#endif
#ifdef WITH_SUNDIALS
  case 6:
    return radau_lobatto_step(data, solverInfo);
  case 7:
    return radau_lobatto_step(data, solverInfo);
  case 8:
    return radau_lobatto_step(data, solverInfo);
  case 9:
    return radau_lobatto_step(data, solverInfo);
  case 10:
    return radau_lobatto_step(data, solverInfo);
  case 11:
    return radau_lobatto_step(data, solverInfo);
#endif

  case 1:
    return euler_ex_step(data, solverInfo);
  }
  return 1;
}

/*! \fn initializeSolverData(DATA* data, SOLVER_INFO* solverInfo)
 *
 *  \param [ref] [data]
 *  \param [ref] [solverInfo]
 *
 *  This function initializes solverInfo.
 */
int initializeSolverData(DATA* data, SOLVER_INFO* solverInfo)
{
  int retValue = 0;
  int i;

  SIMULATION_INFO *simInfo = &(data->simulationInfo);
  SIMULATION_DATA *sData = data->localData[0];

  /* initial solverInfo */
  solverInfo->currentTime = simInfo->startTime;
  solverInfo->currentStepSize = simInfo->stepSize;
  solverInfo->laststep = 0;
  solverInfo->solverRootFinding = 0;
  solverInfo->eventLst = allocList(sizeof(long));
  solverInfo->didEventStep = 0;
  solverInfo->stateEvents = 0;
  solverInfo->sampleEvents = 0;

  if(solverInfo->solverMethod == 2)
  {
    /* Allocate RK work arrays */
    RK4_DATA* rungeData = (RK4_DATA*) malloc(sizeof(RK4_DATA));
    rungeData->work_states_ndims = rungekutta_s;
    rungeData->work_states = (double**) malloc((rungeData->work_states_ndims + 1) * sizeof(double*));
    for(i = 0; i < rungeData->work_states_ndims + 1; i++)
      rungeData->work_states[i] = (double*) calloc(data->modelData.nStates, sizeof(double));
    solverInfo->solverData = rungeData;
  }
  else if(solverInfo->solverMethod == 3)
  {
    /* Initial DASSL solver */
    DASSL_DATA* dasslData = (DASSL_DATA*) malloc(sizeof(DASSL_DATA));
    infoStreamPrint(LOG_SOLVER, 0, "Initializing DASSL");
    retValue = dasrt_initial(data, solverInfo, dasslData);
    solverInfo->solverData = dasslData;
  }
  else if(solverInfo->solverMethod == 4)
  {
    /* Enable inlining solvers */
    work_states = (double**) malloc(inline_work_states_ndims * sizeof(double*));
    for(i = 0; i < inline_work_states_ndims; i++)
      work_states[i] = (double*) calloc(data->modelData.nVariablesReal, sizeof(double));
  }
#ifdef WITH_IPOPT
  else if(solverInfo->solverMethod == 5)
  {
    solverInfo->solverData = malloc(1*sizeof(IPOPT_DATA_));
  }
#endif
#ifdef WITH_SUNDIALS
  else if(solverInfo->solverMethod == 6)
  {
    /* Allocate Radau5 IIA work arrays */
    solverInfo->solverData = calloc(1, sizeof(KINODE));
    allocateKinOde(data, solverInfo, 6, 3);
  }
  else if(solverInfo->solverMethod == 7)
  {
    /* Allocate Radau3 IIA work arrays */
    solverInfo->solverData = calloc(1, sizeof(KINODE));
    allocateKinOde(data, solverInfo, 7, 2);
  }
  else if(solverInfo->solverMethod == 8)
  {
    /* Allocate Radau1 IIA work arrays */
    solverInfo->solverData = calloc(1, sizeof(KINODE));
    allocateKinOde(data, solverInfo, 8, 1);
  }
  else if(solverInfo->solverMethod == 9)
  {
    /* Allocate Lobatto2 IIIA work arrays */
    solverInfo->solverData = calloc(1, sizeof(KINODE));
    allocateKinOde(data, solverInfo, 9, 1);
  }
  else if(solverInfo->solverMethod == 10)
  {
    /* Allocate Lobatto4 IIIA work arrays */
    solverInfo->solverData = calloc(1, sizeof(KINODE));
    allocateKinOde(data, solverInfo, 10, 2);
  }
  else if(solverInfo->solverMethod == 11)
  {
    /* Allocate Lobatto6 IIIA work arrays */
    solverInfo->solverData = calloc(1, sizeof(KINODE));
    allocateKinOde(data, solverInfo, 11, 3);
  }
#endif


  if(measure_time_flag)
  {
    rt_accumulate(SIM_TIMER_PREINIT);
    rt_tick(SIM_TIMER_INIT);
  }

  return retValue;
}

/*! \fn freeSolver(DATA* data, SOLVER_INFO* solverInfo)
 *
 *  \param [ref] [data]
 *  \param [ref] [solverInfo]
 *
 *  This function frees solverInfo.
 */
int freeSolverData(DATA* data, SOLVER_INFO* solverInfo)
{
  int retValue = 0;
  int i;

  /* deintialize solver related workspace */
  if(solverInfo->solverMethod == 2)
  {
    /* free RK work arrays */
    for(i = 0; i < ((RK4_DATA*)(solverInfo->solverData))->work_states_ndims + 1; i++)
      free(((RK4_DATA*)(solverInfo->solverData))->work_states[i]);
    free(((RK4_DATA*)(solverInfo->solverData))->work_states);
    free((RK4_DATA*)solverInfo->solverData);
  }
  else if(solverInfo->solverMethod == 3)
  {
    /* De-Initial DASSL solver */
    dasrt_deinitial(solverInfo->solverData);
  }
  else if(solverInfo->solverMethod == 4)
  {
    /* De-Initial inline solver */
    for(i = 0; i < inline_work_states_ndims; i++)
      free(work_states[i]);
    free(work_states);
  }
#ifdef WITH_IPOPT
  else if(solverInfo->solverMethod == 5)
  {
    /* free  work arrays */
    /*destroyIpopt(solverInfo);*/
  }
#endif
#ifdef WITH_SUNDIALS
  else if(solverInfo->solverMethod == 6)
  {
    /* free  work arrays */
    freeKinOde(data, solverInfo, 6, 3);
  }
  else if(solverInfo->solverMethod == 7)
  {
    /* free  work arrays */
    freeKinOde(data, solverInfo, 7, 2);
  }
  else if(solverInfo->solverMethod == 8)
  {
    /* free  work arrays */
    freeKinOde(data, solverInfo, 8, 1);
  }
  else if(solverInfo->solverMethod == 9)
  {
    /* free  work arrays */
    freeKinOde(data, solverInfo, 9, 1);
  }
  else if(solverInfo->solverMethod == 10)
  {
    /* free  work arrays */
    freeKinOde(data, solverInfo, 10, 2);
  }
  else if(solverInfo->solverMethod == 11)
  {
    /* free  work arrays */
    freeKinOde(data, solverInfo, 11, 3);
  }
#endif
  {
    /* free other solver memory */
  }

  /* free stateset data */
  freeStateSetData(data);

  return retValue;
}


/*! \fn initializeModel(DATA* data, const char* init_initMethod,
 *   const char* init_optiMethod, const char* init_file, double init_time,
 *   int lambda_steps)
 *
 *  \param [ref] [data]
 *  \param [in]  [pInitMethod] user defined initialization method
 *  \param [in]  [pOptiMethod] user defined optimization method
 *  \param [in]  [pInitFile] extra argument for initialization-method "file"
 *  \param [in]  [initTime] extra argument for initialization-method "file"
 *  \param [in]  [lambda_steps] ???
 *
 *  This function starts the initialization process of the model .
 */
int initializeModel(DATA* data, const char* init_initMethod,
    const char* init_optiMethod, const char* init_file, double init_time,
    int lambda_steps)
{
  int retValue = 0;

  SIMULATION_INFO *simInfo = &(data->simulationInfo);

  copyStartValuestoInitValues(data);

  /* read input vars */
  data->callback->input_function(data);

  data->localData[0]->timeValue = simInfo->startTime;

  /* instance all external Objects */
  data->callback->callExternalObjectConstructors(data);

  /* allocate memory for state selection */
  initializeStateSetJacobians(data);

  /* set tolerance for ZeroCrossings */
  setZCtol(simInfo->tolerance);


  simInfo->errorHandler.currentErrorStage = ERROR_SIMULATION;
  /* try */
  if(!setjmp(simInfo->errorHandler.simulationJumpBuffer)) {
    if(initialization(data, init_initMethod, init_optiMethod, init_file, init_time, lambda_steps)) {
      warningStreamPrint(LOG_STDOUT, 0, "Error in initialization. Storing results and exiting.\nUse -lv=LOG_INIT -w for more information.");
      simInfo->stopTime = simInfo->startTime;
      retValue = -1;
    }

    storePreValues(data);
    storeOldValues(data);
    data->callback->function_storeDelayed(data);
    data->callback->function_updateRelations(data, 1);
    storeRelations(data);
    updateHysteresis(data);
    saveZeroCrossings(data);
  } else { /* catch */
    retValue =  -1;
    infoStreamPrint(LOG_STDOUT, 0, "model terminate | Simulation terminated by an assert at initialization");
  }

  /* adrpo: write the parameter data in the file once again after bound parameters and initialization! */
  sim_result.writeParameterData(&sim_result,data);
  infoStreamPrint(LOG_SOLVER, 0, "Wrote parameters to the file after initialization (for output formats that support this)");

  /* Initialization complete */
  if (measure_time_flag) {
    rt_accumulate( SIM_TIMER_INIT);
  }

  return retValue;
}


/*! \fn finishSimulation(DATA* data, SOLVER_INFO* solverInfo)
 *
 *  \param [ref] [data]
 *  \param [ref] [solverInfo]
 *
 *  This function performs the last step
 *  and outputs some statistics, this this simulation terminal step.
 */
int finishSimulation(DATA* data, SOLVER_INFO* solverInfo, const char* outputVariablesAtEnd)
{
  int retValue = 0;
  int ui;

  SIMULATION_INFO *simInfo = &(data->simulationInfo);

  /* Last step with terminal()=true */
  if(solverInfo->currentTime >= simInfo->stopTime && solverInfo->solverMethod != S_OPTIMIZATION)
  {

    infoStreamPrint(LOG_EVENTS_V, 0, "terminal event at stop time %g", solverInfo->currentTime);
    data->simulationInfo.terminal = 1;
    updateDiscreteSystem(data);
    
    /* prevent emit if noeventemit flag is used */
    if (!(omc_flag[FLAG_NOEVENTEMIT]))
      sim_result.emit(&sim_result,data);

    data->simulationInfo.terminal = 0;
  }
  communicateStatus("Finished", 1);

  /* we have output variables in the command line -output a,b,c */
  if(outputVariablesAtEnd)
  {
    writeOutputVars(strdup(outputVariablesAtEnd), data);
  }

  if(ACTIVE_STREAM(LOG_STATS))
  {
    rt_accumulate(SIM_TIMER_TOTAL);

    infoStreamPrint(LOG_STATS, 0, "### STATISTICS ###");

    infoStreamPrint(LOG_STATS, 1, "timer");
    infoStreamPrint(LOG_STATS, 0, "%12gs [%5.1f%%] pre-initialization", rt_accumulated(SIM_TIMER_PREINIT), rt_accumulated(SIM_TIMER_PREINIT)/rt_accumulated(SIM_TIMER_TOTAL)*100.0);
    infoStreamPrint(LOG_STATS, 0, "%12gs [%5.1f%%] initialization", rt_accumulated(SIM_TIMER_INIT), rt_accumulated(SIM_TIMER_INIT)/rt_accumulated(SIM_TIMER_TOTAL)*100.0);
    infoStreamPrint(LOG_STATS, 0, "%12gs [%5.1f%%] steps", rt_accumulated(SIM_TIMER_STEP), rt_accumulated(SIM_TIMER_STEP)/rt_accumulated(SIM_TIMER_TOTAL)*100.0);
    infoStreamPrint(LOG_STATS, 0, "%12gs [%5.1f%%] creating output-file", rt_accumulated(SIM_TIMER_OUTPUT), rt_accumulated(SIM_TIMER_OUTPUT)/rt_accumulated(SIM_TIMER_TOTAL)*100.0);
    infoStreamPrint(LOG_STATS, 0, "%12gs [%5.1f%%] event-handling", rt_accumulated(SIM_TIMER_EVENT), rt_accumulated(SIM_TIMER_EVENT)/rt_accumulated(SIM_TIMER_TOTAL)*100.0);
    infoStreamPrint(LOG_STATS, 0, "%12gs [%5.1f%%] overhead", rt_accumulated(SIM_TIMER_OVERHEAD), rt_accumulated(SIM_TIMER_OVERHEAD)/rt_accumulated(SIM_TIMER_TOTAL)*100.0);

    if(solverInfo->solverMethod != S_OPTIMIZATION)
      infoStreamPrint(LOG_STATS, 0, "%12gs [%5.1f%%] simulation", rt_accumulated(SIM_TIMER_TOTAL)-rt_accumulated(SIM_TIMER_OVERHEAD)-rt_accumulated(SIM_TIMER_EVENT)-rt_accumulated(SIM_TIMER_OUTPUT)-rt_accumulated(SIM_TIMER_STEP)-rt_accumulated(SIM_TIMER_INIT)-rt_accumulated(SIM_TIMER_PREINIT), (rt_accumulated(SIM_TIMER_TOTAL)-rt_accumulated(SIM_TIMER_OVERHEAD)-rt_accumulated(SIM_TIMER_EVENT)-rt_accumulated(SIM_TIMER_OUTPUT)-rt_accumulated(SIM_TIMER_STEP)-rt_accumulated(SIM_TIMER_INIT)-rt_accumulated(SIM_TIMER_PREINIT))/rt_accumulated(SIM_TIMER_TOTAL)*100.0);
    else
      infoStreamPrint(LOG_STATS, 0, "%12gs [%5.1f%%] optimization", rt_accumulated(SIM_TIMER_TOTAL)-rt_accumulated(SIM_TIMER_OVERHEAD)-rt_accumulated(SIM_TIMER_EVENT)-rt_accumulated(SIM_TIMER_OUTPUT)-rt_accumulated(SIM_TIMER_STEP)-rt_accumulated(SIM_TIMER_INIT)-rt_accumulated(SIM_TIMER_PREINIT), (rt_accumulated(SIM_TIMER_TOTAL)-rt_accumulated(SIM_TIMER_OVERHEAD)-rt_accumulated(SIM_TIMER_EVENT)-rt_accumulated(SIM_TIMER_OUTPUT)-rt_accumulated(SIM_TIMER_STEP)-rt_accumulated(SIM_TIMER_INIT)-rt_accumulated(SIM_TIMER_PREINIT))/rt_accumulated(SIM_TIMER_TOTAL)*100.0);

    infoStreamPrint(LOG_STATS, 0, "%12gs [%5.1f%%] total", rt_accumulated(SIM_TIMER_TOTAL), rt_accumulated(SIM_TIMER_TOTAL)/rt_accumulated(SIM_TIMER_TOTAL)*100.0);
    messageClose(LOG_STATS);
    
    infoStreamPrint(LOG_STATS, 1, "events");
    infoStreamPrint(LOG_STATS, 0, "%5ld state events", solverInfo->stateEvents);
    infoStreamPrint(LOG_STATS, 0, "%5ld time events", solverInfo->sampleEvents);
    messageClose(LOG_STATS);

    if(S_DASSL == solverInfo->solverMethod)
    {
      /* save dassl stats before print */
      for(ui=0; ui<numStatistics; ui++)
        ((DASSL_DATA*)solverInfo->solverData)->dasslStatistics[ui] += ((DASSL_DATA*)solverInfo->solverData)->dasslStatisticsTmp[ui];

      infoStreamPrint(LOG_STATS, 1, "solver: DASSL");
      infoStreamPrint(LOG_STATS, 0, "%5d steps taken", ((DASSL_DATA*)solverInfo->solverData)->dasslStatistics[0]);
      infoStreamPrint(LOG_STATS, 0, "%5d calls of functionODE", ((DASSL_DATA*)solverInfo->solverData)->dasslStatistics[1]);
      infoStreamPrint(LOG_STATS, 0, "%5d evaluations of jacobian", ((DASSL_DATA*)solverInfo->solverData)->dasslStatistics[2]);
      infoStreamPrint(LOG_STATS, 0, "%5d error test failures", ((DASSL_DATA*)solverInfo->solverData)->dasslStatistics[3]);
      infoStreamPrint(LOG_STATS, 0, "%5d convergence test failures", ((DASSL_DATA*)solverInfo->solverData)->dasslStatistics[4]);
      messageClose(LOG_STATS);
    }
    else if(S_OPTIMIZATION == solverInfo->solverMethod)
    {
      /* skip solver statistics for optimization */
    }
    else
    {
      infoStreamPrint(LOG_STATS, 1, "solver");
      infoStreamPrint(LOG_STATS, 0, "sorry - no solver statistics available. [not yet implemented]");
      messageClose(LOG_STATS);
    }

    infoStreamPrint(LOG_STATS, 0, "### END STATISTICS ###");

    rt_tick(SIM_TIMER_TOTAL);
  }
  return retValue;
}

/*! \fn solver_main
 *
 *  \param [ref] [data]
 *  \param [in]  [pInitMethod] user defined initialization method
 *  \param [in]  [pOptiMethod] user defined optimization method
 *  \param [in]  [pInitFile] extra argument for initialization-method "file"
 *  \param [in]  [initTime] extra argument for initialization-method "file"
 *  \param [in]  [lambda_steps] ???
 *  \param [in]  [solverID] selects the ode solver
 *  \param [in]  [outputVariablesAtEnd] ???
 *
 *  This is the main function of the solver it perform the simulation.
 */
int solver_main(DATA* data, const char* init_initMethod,
    const char* init_optiMethod, const char* init_file, double init_time,
    int lambda_steps, int solverID, const char* outputVariablesAtEnd)
{
  int i, retVal = 0;
  unsigned int ui;
  SOLVER_INFO solverInfo;
  SIMULATION_INFO *simInfo = &(data->simulationInfo);

  solverInfo.solverMethod = solverID;
  
  /* do some solver specific checks */
  switch(solverInfo.solverMethod)
  {
  case S_DASSLWORT:
  case S_DASSLTEST:
  case S_DASSLSYMJAC:
  case S_DASSLNUMJAC:
  case S_DASSLCOLORSYMJAC:
  case S_DASSLINTERNALNUMJAC:
    solverInfo.solverMethod = S_DASSL;
    break;

#ifndef WITH_SUNDIALS
  case S_RADAU1:
  case S_RADAU3:
  case S_RADAU5:
  case S_LOBATTO2:
  case S_LOBATTO4:
  case S_LOBATTO6:
    warningStreamPrint(LOG_STDOUT, 0, "Sundial/kinsol is needed but not available. Please choose other solver.");
    return 1;
#endif

#ifndef WITH_IPOPT
  case S_OPTIMIZATION:
    warningStreamPrint(LOG_STDOUT, 0, "Ipopt is needed but not available.");
    return 1;
#endif
    

  case S_INLINE_EULER:
    if(!_omc_force_solver || strcmp(_omc_force_solver, "inline-euler"))
    {
      infoStreamPrint(LOG_SOLVER, 0, "Recognized solver: inline-euler, but the executable was not compiled with support for it. Compile with -D_OMC_INLINE_EULER.");
      return 1;
    }
    break;
    
  case S_INLINE_RUNGEKUTTA:
    if(!_omc_force_solver || strcmp(_omc_force_solver, "inline-rungekutta"))
    {
      infoStreamPrint(LOG_SOLVER, 0, "Recognized solver: inline-rungekutta, but the executable was not compiled with support for it. Compile with -D_OMC_INLINE_RK.");
      return 1;
    }
    solverInfo.solverMethod = S_INLINE_EULER;
    break;
  }

  /* allocate SolverInfo memory */
  retVal = initializeSolverData(data, &solverInfo);
  omc_alloc_interface.collect_a_little();
  
  /* initialize all parts of the model */
  if(0 == retVal) {
    retVal = initializeModel(data, init_initMethod, init_optiMethod, init_file, init_time, lambda_steps);
  }
  omc_alloc_interface.collect_a_little();

  /* starts the simulation main loop */
  if(0 == retVal)
  {

    if(solverInfo.solverMethod != S_OPTIMIZATION)
    {
      sim_result.emit(&sim_result,data);
      overwriteOldSimulationData(data);
    }

    infoStreamPrint(LOG_SOLVER, 0, "Start numerical solver from %g to %g", simInfo->startTime, simInfo->stopTime);
    retVal = data->callback->performSimulation(data, &solverInfo);
    omc_alloc_interface.collect_a_little();
    /* terminate the simulation */
    finishSimulation(data, &solverInfo, outputVariablesAtEnd);
    omc_alloc_interface.collect_a_little();
  }

  /* free SolverInfo memory */
  freeSolverData(data, &solverInfo);

  return retVal;
}

/***************************************    EULER_EXP     *********************************/
static int euler_ex_step(DATA* data, SOLVER_INFO* solverInfo)
{
  int i;
  SIMULATION_DATA *sData = (SIMULATION_DATA*)data->localData[0];
  SIMULATION_DATA *sDataOld = (SIMULATION_DATA*)data->localData[1];
  modelica_real* stateDer = sDataOld->realVars + data->modelData.nStates;

  solverInfo->currentTime = sDataOld->timeValue + solverInfo->currentStepSize;

  for(i = 0; i < data->modelData.nStates; i++)
  {
    sData->realVars[i] = sDataOld->realVars[i] + stateDer[i] * solverInfo->currentStepSize;
  }
  sData->timeValue = solverInfo->currentTime;
  return 0;
}

/***************************************    RK4      ***********************************/
static int rungekutta_step(DATA* data, SOLVER_INFO* solverInfo)
{
  double** k = ((RK4_DATA*)(solverInfo->solverData))->work_states;
  double sum;
  int i,j;
  SIMULATION_DATA *sData = (SIMULATION_DATA*)data->localData[0];
  SIMULATION_DATA *sDataOld = (SIMULATION_DATA*)data->localData[1];
  modelica_real* stateDer = sData->realVars + data->modelData.nStates;
  modelica_real* stateDerOld = sDataOld->realVars + data->modelData.nStates;

  solverInfo->currentTime = sDataOld->timeValue + solverInfo->currentStepSize;

  /* We calculate k[0] before returning from this function.
   * We only want to calculate f() 4 times per call */
  for(i = 0; i < data->modelData.nStates; i++)
  {
    k[0][i] = stateDerOld[i];
  }

  for(j = 1; j < rungekutta_s; j++)
  {
    for(i = 0; i < data->modelData.nStates; i++)
    {
      sData->realVars[i] = sDataOld->realVars[i] + solverInfo->currentStepSize * rungekutta_c[j] * k[j - 1][i];
    }
    sData->timeValue = sDataOld->timeValue + rungekutta_c[j] * solverInfo->currentStepSize;
    data->callback->functionODE(data);
    for(i = 0; i < data->modelData.nStates; i++)
    {
      k[j][i] = stateDer[i];
    }
  }

  for(i = 0; i < data->modelData.nStates; i++)
  {
    sum = 0;
    for(j = 0; j < rungekutta_s; j++)
    {
      sum = sum + rungekutta_b[j] * k[j][i];
    }
    sData->realVars[i] = sDataOld->realVars[i] + solverInfo->currentStepSize * sum;
  }
  sData->timeValue = solverInfo->currentTime;
  return 0;
}

/***************************************    Run Ipopt for optimization     ***********************************/
#ifdef WITH_IPOPT
static int ipopt_step(DATA* data, SOLVER_INFO* solverInfo)
{
  int cJ = data->simulationInfo.errorHandler.currentErrorStage;
  data->simulationInfo.errorHandler.currentErrorStage = ERROR_OPTIMIZE;
  startIpopt(data, solverInfo,5);
  data->simulationInfo.errorHandler.currentErrorStage = cJ;
  return 0;
}
#endif

#ifdef WITH_SUNDIALS
/***************************************    Radau/Lobatto     ***********************************/
int radau_lobatto_step(DATA* data, SOLVER_INFO* solverInfo)
{
  if(kinsolOde(solverInfo->solverData) == 0)
  {
    solverInfo->currentTime += solverInfo->currentStepSize;
    return 0;
  }
  return -1; 
}
#endif


static void writeOutputVars(char* names, DATA* data)
{
  int i = 0;
  char *p = strtok(names, ",");

  fprintf(stdout, "time=%.20g", data->localData[0]->timeValue);

  while(p)
  {
    for(i = 0; i < data->modelData.nVariablesReal; i++)
      if(!strcmp(p, data->modelData.realVarsData[i].info.name))
        fprintf(stdout, ",%s=%.20g", p, (data->localData[0])->realVars[i]);
    for(i = 0; i < data->modelData.nVariablesInteger; i++)
      if(!strcmp(p, data->modelData.integerVarsData[i].info.name))
        fprintf(stdout, ",%s=%li", p, (data->localData[0])->integerVars[i]);
    for(i = 0; i < data->modelData.nVariablesBoolean; i++)
      if(!strcmp(p, data->modelData.booleanVarsData[i].info.name))
        fprintf(stdout, ",%s=%i", p, (data->localData[0])->booleanVars[i]);
    for(i = 0; i < data->modelData.nVariablesString; i++)
      if(!strcmp(p, data->modelData.stringVarsData[i].info.name))
        fprintf(stdout, ",%s=\"%s\"", p, (data->localData[0])->stringVars[i]);

    for(i = 0; i < data->modelData.nAliasReal; i++)
      if(!strcmp(p, data->modelData.realAlias[i].info.name))
      {
       if(data->modelData.realAlias[i].negate)
         fprintf(stdout, ",%s=%.20g", p, -(data->localData[0])->realVars[data->modelData.realAlias[i].nameID]);
       else
         fprintf(stdout, ",%s=%.20g", p, (data->localData[0])->realVars[data->modelData.realAlias[i].nameID]);
      }
    for(i = 0; i < data->modelData.nAliasInteger; i++)
      if(!strcmp(p, data->modelData.integerAlias[i].info.name))
      {
        if(data->modelData.integerAlias[i].negate)
          fprintf(stdout, ",%s=%li", p, -(data->localData[0])->integerVars[data->modelData.integerAlias[i].nameID]);
        else
          fprintf(stdout, ",%s=%li", p, (data->localData[0])->integerVars[data->modelData.integerAlias[i].nameID]);
      }
    for(i = 0; i < data->modelData.nAliasBoolean; i++)
      if(!strcmp(p, data->modelData.booleanAlias[i].info.name))
      {
        if(data->modelData.booleanAlias[i].negate)
          fprintf(stdout, ",%s=%i", p, -(data->localData[0])->booleanVars[data->modelData.booleanAlias[i].nameID]);
        else
          fprintf(stdout, ",%s=%i", p, (data->localData[0])->booleanVars[data->modelData.booleanAlias[i].nameID]);
      }
    for(i = 0; i < data->modelData.nAliasString; i++)
      if(!strcmp(p, data->modelData.stringAlias[i].info.name))
        fprintf(stdout, ",%s=\"%s\"", p, (data->localData[0])->stringVars[data->modelData.stringAlias[i].nameID]);

    /* parameters */
    for(i = 0; i < data->modelData.nParametersReal; i++)
      if(!strcmp(p, data->modelData.realParameterData[i].info.name))
        fprintf(stdout, ",%s=%.20g", p, data->simulationInfo.realParameter[i]);

    for(i = 0; i < data->modelData.nParametersInteger; i++)
      if(!strcmp(p, data->modelData.integerParameterData[i].info.name))
        fprintf(stdout, ",%s=%li", p, data->simulationInfo.integerParameter[i]);

    for(i = 0; i < data->modelData.nParametersBoolean; i++)
      if(!strcmp(p, data->modelData.booleanParameterData[i].info.name))
        fprintf(stdout, ",%s=%i", p, data->simulationInfo.booleanParameter[i]);

    for(i = 0; i < data->modelData.nParametersString; i++)
      if(!strcmp(p, data->modelData.stringParameterData[i].info.name))
        fprintf(stdout, ",%s=\"%s\"", p, data->simulationInfo.stringParameter[i]);

    /* move to next */
    p = strtok(NULL, ",");
  }
  fprintf(stdout, "\n"); fflush(stdout);
}
