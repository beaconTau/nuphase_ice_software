/* Main Acquisition program for vPhase 
 *
 *
 * Cosmin Deaconu <cozzyd@kicp.uchicago.edu>
 *
 *
 * Design: 
 *
 * Despite running on a single(at least for the first iteration) processor, the
 * work is divided into multiple threads to reduce latency of critical tasks. 
 *
 * Right now the threads are: 
 *
 *  - The main thread: reads the config, sets up, tears down, and waits for
 *  signals. 
 *
 * - Acquisition thread - takes the data and reads it
 * 
 * - A monitoring thread, reads in statuses, send software_triggers, and does the pid loops 
 *   (not sure if this should be merged into the acquisition thread...) 
 *
 * - A write thread, which writes to disk and screen.
 *
 * The config is read on startup. Right now, the configuration cannot be reloaded
 * without a restart.
 *
 **/ 



/************** Includes **********************************/

#include "beacon-common.h"
#include "beacon-cfg.h"
#include "beaconhk.h" 
#include "beacon-buf.h" 
#include "beacondaq.h"
#include <pthread.h> 
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/mman.h> 
#include <fcntl.h> 
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <inttypes.h> 
#include <math.h> 


/************** Structs /Typedefs ******************************/


/** PID state */ 
typedef struct pid_state
{
  double k_p; 
  double k_i; 
  double k_d; 
  int nsum; 
  double error[BN_NUM_BEAMS] ; 
  double sum_error[BN_NUM_BEAMS];
  double last_measured[BN_NUM_BEAMS];
} pid_state_t; 

static void pid_state_init (pid_state_t * c, double p, double i, double d)
{
  c->k_p = p; 
  c->k_i = i; 
  c->k_d = d; 

  c->nsum =0;
  memset(c->sum_error,0, sizeof(c->sum_error)); 
  memset(c->last_measured,0, sizeof(c->last_measured)); 
  memset(c->error,0, sizeof(c->error)); 
}

static void pid_state_print(FILE * f, const pid_state_t * pid) 
{

  fprintf(f,"===PID STATE (k_p=%g, k_i=%g, k_d=%g, n: %d)) \n", pid->k_p, pid->k_i, pid->k_d, pid->nsum); 
  int ibeam; 
  for (ibeam = 0; ibeam < BN_NUM_BEAMS; ibeam++)
  {
    fprintf(f,"   Beam %d :: error: %g ::  sum_error: %g ::  last_measured: %g\n",ibeam , pid->error[ibeam], pid->sum_error[ibeam], pid->last_measured[ibeam] );  
  }

}


/* This is what is stored within the acquisition buffer 
 *
 * It's a bit wasteful... we allocate space for all buffers even though
 * we often (usually? )won't have all of them. 
 *
 * Will see if this is a problem. 
 **/ 
typedef struct acq_buffer
{
  int nfilled; 
  beacon_event_t events[BN_NUM_BUFFER]; 
  beacon_header_t headers[BN_NUM_BUFFER]; 
} acq_buffer_t;


/* this is what is stored within the monitor buffer */ 
typedef struct monitor_buffer
{
  beacon_status_t status; //status before
  uint32_t thresholds[BN_NUM_BEAMS]; //thresholds when written 
  pid_state_t control; //the pid state 
} monitor_buffer_t; 

/**************Static vars *******************************/

/* The configuration state */ 
static beacon_acq_cfg_t config; 

/** Need this to apply attenuation */
static beacon_start_cfg_t start_config; 

/** Need this to read hk */
static beacon_hk_cfg_t hk_config; 

static beacon_hk_t * the_hk = 0; 


/* Mutex protecting the configuration */
static pthread_mutex_t config_lock = PTHREAD_MUTEX_INITIALIZER; 

/* The device */
static beacon_dev_t* device;

/* Acquisition thread handles */ 
static pthread_t the_acq_thread; 

/*Buffer for acq thread */ 
static beacon_buf_t* acq_buffer = 0; 

/*Buffer for monitor thread */ 
static beacon_buf_t* mon_buffer = 0; 

/* Monitor thread handle */ 
static pthread_t the_mon_thread; 

/* Write thread handle */ 
static pthread_t the_wri_thread; 

static pid_state_t control; 

static int status_save_fd = -1; 
static beacon_status_t * saved_status = 0; 

/* used for exiting */ 
static volatile int die; 


/************** Prototypes *********************
 Brief documentation follows. More detailed documentation
 at implementation.
 */


static int run_number; 


// this sets everything up (opens device, starts threads, signal handlers, etc. ) 
//
//
const int SETUP_FAILED = 1; 
const int SETUP_TRY_AGAIN_LATER = 2; 
static int setup(); 
// this cleans up 
static int teardown(); 

//this reads in the config. Some things may be changed later. 
static int read_config(int first_time); 


// call this when we need to stop
static void fatal(); 

static void signal_handler(int, siginfo_t *, void *); 

/* Acquisition thread */ 
static void * acq_thread(void * p);

/* Monitor thread */ 
static void * monitor_thread(void * p); 

/* Write thread */ 
static void * write_thread(void * p ); 



/* The main function... not too much here 
 *   Just check if it's time to pack our bags and let another run take care of things. 
 * */ 
int main(int nargs, char ** args) 
{


  int setup_return = setup(); 

  if (setup_return == SETUP_FAILED) 
  {
    return 1; //this is bad
  }

  if (setup_return == SETUP_TRY_AGAIN_LATER) 
  {
    sleep(config.try_again_sleep_amount); 
    return 0; 
  }

  struct timespec start; 
  clock_gettime(CLOCK_MONOTONIC_COARSE, &start); 


  /** Keeps track of the last time we checked the power information */ 
  struct timespec last_check_power = {.tv_sec = 0, .tv_nsec = 0}; 

  /** Keeps track of how many times in a row there is no power info (battery voltage is 0) */ 
  int num_no_power_info = 0; 

  int must_turn_off = 0;
 


  struct timespec now; 
  while(!die) 
  {
    clock_gettime(CLOCK_MONOTONIC_COARSE, &now); 
    if ( now.tv_sec - start.tv_sec > config.run_length) 
    {
      fatal(); 
    }

    usleep(500000);  //500 ms 

    /*Power checks go here  */ 
    if (the_hk && (config.auto_power_off || config.check_power_on)  && (timespec_difference_float(&now, &last_check_power) > config.power_monitor_interval))
    {

      int must_turn_off = 0; 


      lock_shared_hk(); 
      int adc_current = the_hk->adc_current; 
      float cc_voltage = the_hk->cc_batt_dV*10; 
      float inv_voltage = the_hk->inv_batt_dV*10; 
      unlock_shared_hk(); 

      if (config.check_power_on && adc_current < config.adc_threshold_for_on) 
      {
        fprintf(stderr,"WARNING: was the system turned off!???"); 
        fatal(); 
        must_turn_off=1; //just in case... 


      }

      if (config.auto_power_off) 
      {
        if ( (cc_voltage = 0 || inv_voltage == 0) && ( ++num_no_power_info > config.nzero_threshold_to_turn_off))
        {
          must_turn_off = 1; 
          fprintf(stderr,"max number of zero values of battery voltage exceeded! turning off!...\n"); 
        }
        else if (cc_voltage < config.cc_voltage_to_turn_off || inv_voltage < config.inv_voltage_to_turn_off)
        {
          must_turn_off = 1; 
          fprintf(stderr,"battery voltage too low! turning off...\n"); 
        }

        if (must_turn_off) 
        {
          fatal();
        }
      }

      clock_gettime(CLOCK_MONOTONIC_COARSE, &last_check_power); 
    }

    if (the_hk && the_hk->disk_space_kB < 1e6)
    {
      fprintf(stderr,"Less than 1 GB of disk space remaining.... quitting!\n"); 
      fatal(); 
    }



    sched_yield();
  }

  int teardown_return = teardown(); 

  if (must_turn_off) teardown_return += system(config.power_off_command); 

  return teardown_return; 

}



/*** Acquistion thread 
 *
 * This will wait for data, then record and it and put it into a memory buffer, awaiting to be written to disk. 
 *
 * It's remarkably simple since beacondaq.so does all the hard work. 
 *
 ***/ 
void * acq_thread(void *v) 
{


  while(!die) 
  {
   /* grab a buffer to fill */ 
    acq_buffer_t * mem = (acq_buffer_t*) beacon_buf_getmem(acq_buffer); 
    mem->nfilled = 0; //nothing filled

    while (!mem->nfilled && !die) 
    {
      mem->nfilled = beacon_wait_for_and_read_multiple_events(device, &mem->headers, &mem->events); 
    }
    beacon_buf_commit(acq_buffer); // we filled it 
  }

  return 0; 
}



///////////////////////////////////////////////////
// stuff for keeping track of fast scaler averages 

static struct
{
  uint16_t * buf[BN_NUM_BEAMS]; 
  uint32_t sum[BN_NUM_BEAMS]; 
  size_t sz; 
  size_t i; 
} fs_avg = { .buf = {0}, .sum={0}, .sz = 0, .i = 0 }; 




static void fs_avg_init(int n) 
{
  int ibeam = 0;

  for (ibeam = 0; ibeam < BN_NUM_BEAMS; ibeam++)
  {
    fs_avg.buf[ibeam] = calloc(n * sizeof(*fs_avg.buf[ibeam]),1); 
    fs_avg.sum[ibeam] = 0;
  }

 fs_avg.sz = n; 
 fs_avg.i = 0; 
}

static void fs_avg_add(const beacon_status_t *st) 
{
  int ibeam = 0; 
  for (ibeam = 0; ibeam < BN_NUM_BEAMS; ibeam++)
  {
    uint16_t val = st->beam_scalers[SCALER_FAST][ibeam]; 
    fs_avg.sum[ibeam] -= fs_avg.buf[ibeam][fs_avg.i % fs_avg.sz];
    fs_avg.sum[ibeam] += val; 
    fs_avg.buf[ibeam][fs_avg.i % fs_avg.sz] = val; 
  }
  fs_avg.i++;
}

static double fs_avg_get(int ibeam) 
{
  int max = fs_avg.i < fs_avg.sz ? fs_avg.i : fs_avg.sz; 
  return ((double) fs_avg.sum[ibeam]) / max; 
}

static void fs_avg_print(FILE * f) 
{
  int ibeam;
  fprintf(f,"Running average of %zu fast scalers:\n\t", fs_avg.i < fs_avg.sz ? fs_avg.i : fs_avg.sz); 
  for (ibeam = 0; ibeam < BN_NUM_BEAMS; ibeam++) 
  {
    fprintf(f,"%0.4g  ", fs_avg_get(ibeam) ); 
  }
  fprintf(f,"\n"); 
}

///
/////////////////////////////////////////////////////



static struct drand48_data sw_rand; 
static double get_next_sw_trig_interval()
{
  if (!config.sw_trigger_interval) return 0; 
  if (config.randomize_sw_trigger)
  {
    double u = 0; 
    while (u==1 || u==0)  //make sure we don't have 0 or 1
    {
      drand48_r(&sw_rand,&u); 
    }

    return -log(u)*config.sw_trigger_interval; 
  }

  return config.sw_trigger_interval; 

}


/***********************************************************************
 * Monitor thread
 *
 * This will periodically grab the status / adjust thresholds / send software triggers
 *
 *  The PID loops lies within here. 
 ***************************************************************************************/
void * monitor_thread(void *v) 
{

  //The start time 
  struct timespec start; 
  clock_gettime(CLOCK_MONOTONIC, &start); 

  //seed the rng for randomizing sw trigger
  srand48_r(start.tv_nsec, &sw_rand); 

  // this keeps track of when we sent the last sw trigger
  struct timespec last_sw_trig = { .tv_sec = 0, .tv_nsec = 0};

  //this keeps track of the last time we monitored
  struct timespec last_mon = { .tv_sec = 0, .tv_nsec = 0}; //dont' read scalers yet! 

  // turn off verification, a hacky work around for now...
  beacon_enable_verification_mode(device, 0);

  // the phased trigger status, so that we can turn it on or off as appropriate
  // start as undefined
  int phased_trigger_status = -1; 

  double sw_trig_interval =  get_next_sw_trig_interval(); 

  while(!die) 
  {
    //figure out the current time
    struct timespec now; 
    clock_gettime(CLOCK_MONOTONIC, &now); 


    /////////////////////////////////////////////////////
    //turn on and off phased trigger as necessary
    //  this is more complicated than it could be because the config
    //  could be reread in the middle of the run. 
    /////////////////////////////////////////////////////
    if (config.enable_phased_trigger && phased_trigger_status != 1)
    {
      if (config.secs_before_phased_trigger)
      {
        struct timespec now; 
        clock_gettime(CLOCK_MONOTONIC, &now); 
        if (timespec_difference_float(&now,&start) > config.secs_before_phased_trigger)
        {
          beacon_phased_trigger_readout(device, 1); 
          phased_trigger_status = 1; 
        }
      }
      else
      {
        beacon_phased_trigger_readout(device, 1); 
        phased_trigger_status = 1; 
      }
    }
    else if (!config.enable_phased_trigger && phased_trigger_status == 1)
    {
      beacon_phased_trigger_readout(device, 0); 
      phased_trigger_status = 0; 
    }

 
    /// Figure out how long it's been since last time we monitored and sent a software trigger
    float diff_mon = timespec_difference_float(&now, &last_mon); 
    float diff_swtrig = timespec_difference_float(&now, &last_sw_trig); 

    //////////////////////////////////////////////////////
    //read the status, and react to it 
    // herein lies the PID loop and all those wonderful things
    //////////////////////////////////////////////////////
    if (config.monitor_interval &&  diff_mon > config.monitor_interval)
    {
      monitor_buffer_t mb; 
      beacon_status_t *st = &mb.status;

      beacon_read_status(device, st, MASTER); 
 //     beacon_status_print(stdout,st); 

      int ibeam; 
      uint32_t dont_set = 0; 
      if (!config.use_fixed_thresholds) fs_avg_add(st); 

      for (ibeam = 0; ibeam < BN_NUM_BEAMS; ibeam++)
      {

        if (config.use_fixed_thresholds) 
        {
          mb.thresholds[ibeam] = config.fixed_threshold[ibeam]; 
          continue; 
        }
        else if (!config.scaler_goal[ibeam])
        {
          dont_set |= (1 << ibeam); 
          control.last_measured[ibeam]=0; 
          control.sum_error[ibeam]=0;
          control.error[ibeam] = 0; 
          mb.thresholds[ibeam] = st->trigger_thresholds[ibeam]; 
          continue; 
        }
       
        ///// REVISIT THIS 
        ///// We need to figure out how to use both the fast and slow scalers
        ///// For now, take a weighted average of the fast and slow scalers to determine the rate. 
        double measured_slow = ((double) st->beam_scalers[SCALER_SLOW][ibeam]) / BN_SCALER_TIME(SCALER_SLOW); 
        double measured_fast = fs_avg_get(ibeam) / BN_SCALER_TIME(SCALER_FAST); 
        double measured =  (config.slow_scaler_weight * measured_slow + config.fast_scaler_weight * measured_fast) / (config.slow_scaler_weight + config.fast_scaler_weight); 

        if (config.subtract_gated) 
        {
          double measured_gated_slow = ((double) st->beam_scalers[SCALER_SLOW_GATED][ibeam]) / BN_SCALER_TIME(SCALER_SLOW_GATED); 
          measured -= measured_gated_slow; 
        }

        double e =  measured - config.scaler_goal[ibeam]; 
        control.error[ibeam] = e; 
        double de = 0;

        // only compute difference after first iteration
        if (control.nsum > 0) 
        {
          de = (measured - control.last_measured[ibeam]) / diff_mon; 
        }

        control.sum_error[ibeam] += e; 
        control.nsum++; 
        double ie = control.sum_error[ibeam]; 

        control.last_measured[ibeam] = measured; 

        // modify threshold 
        double dthreshold =   control.k_p * e + control.k_i * ie * control.k_d * de; 
        
        //cap the threshold increase at each step 
        if (dthreshold > config.max_threshold_increase) dthreshold = config.max_threshold_increase;

        mb.thresholds[ibeam] = st->trigger_thresholds[ibeam] + dthreshold;

        if(mb.thresholds[ibeam] < config.min_threshold){
          mb.thresholds[ibeam] = config.min_threshold;
        }

//        printf("BEAM %d\n", ibeam); 
//        printf("  slow scaler: %f, fast_scaler: %f, avg: %f\n", measured_slow, measured_fast, measured); 
//        printf("  e: %f, ie: %f , de: %f\n", e,ie,de); 
//        printf("  new threshold %d (old: %d)\n", mb.thresholds[ibeam], st->trigger_thresholds[ibeam]); 
      }

      //apply the thresholds 
      beacon_set_thresholds(device, mb.thresholds,dont_set); 
      
      //copy over the current control status 
      if (!config.use_fixed_thresholds) memcpy(&mb.control, &control, sizeof(control)); 

      beacon_buf_push(mon_buffer, &mb);
      memcpy(&last_mon,&now, sizeof(now)); 
      diff_mon = 0; 
    }

    if (sw_trig_interval && diff_swtrig > sw_trig_interval)
    {
      beacon_sw_trigger(device); 
      memcpy(&last_sw_trig,&now, sizeof(now)); 
      diff_swtrig = 0;
      sw_trig_interval = get_next_sw_trig_interval(); 
    }

    //now figure out how long to sleep 
    //
    float how_long_to_sleep = 0.1; //don't sleep longer than 100 ms 

    if (config.monitor_interval && config.monitor_interval - diff_mon  < how_long_to_sleep) how_long_to_sleep = config.monitor_interval - diff_mon; 
    if (sw_trig_interval && sw_trig_interval - diff_swtrig  < how_long_to_sleep) how_long_to_sleep = sw_trig_interval - diff_swtrig; 

    usleep(how_long_to_sleep * 1e6); 
  }

  return 0; 
}


const char * subdirs[] = {"event","header","status","aux","cfg"}; 
const int nsubdirs = sizeof(subdirs) / sizeof(*subdirs); 

//this makes the necessary directories for a time 
//returns 0 on success. 
static int make_dirs_for_output(const char * prefix) 
{ 
  char buf[strlen(prefix) + 512];  

  //check to see that prefix exists and is a directory
  if (mkdir_if_needed(prefix))
  {
    fprintf(stderr,"Couldn't find %s or it's not a directory. Bad things will happen!\n",prefix); 
    return 1; 
  }


  int i;

  for (i = 0; i < nsubdirs; i++)
  {
    snprintf(buf,sizeof(buf), "%s/%s",prefix,subdirs[i]); 
    if (mkdir_if_needed(buf))
    {
        fprintf(stderr,"Couldn't make %s. Bad things will happen!\n",buf); 
        return 1; 
    }
  }

  return 0; 
}


static char * output_dir = 0;

void copy_configs() 
{

  char * cfgpath = 0; 
  char bigbuf[1024];
  if (!output_dir) return; 

  beacon_program_t prog;
  for (prog = BEACON_STARTUP; prog <= BEACON_COPY; prog++)
  {
    beacon_get_cfg_file(&cfgpath, prog); 
    snprintf(bigbuf,sizeof(bigbuf), "cp --backup=simple %s %s/cfg", cfgpath, output_dir); 
    system(bigbuf); 
  }

}

/** Will write output to disk and some status info to screen */ 
void * write_thread(void *v) 
{
  time_t start_time = time(0);
  time_t last_print_out =start_time ; 

  char bigbuf[strlen(config.output_directory)+512];

  int    data_file_size = 0;
  int    header_file_size = 0;
  int    status_file_size =0;

  gzFile data_file = 0 ; 
  gzFile header_file = 0 ; 
  gzFile status_file  = 0 ; 
  char * data_file_name = 0; 
  char * header_file_name = 0; 
  char * status_file_name = 0; 

  acq_buffer_t *events= 0; 
  monitor_buffer_t *mon= 0;

  beacon_status_t * last_status = (saved_status && saved_status != MAP_FAILED)  ? saved_status : malloc(sizeof(beacon_status_t)); 

  pid_state_t last_pid; 
  pid_state_init(&last_pid,-1,-1,-1); 

  beacon_read_status(device, last_status,MASTER); 

  
  int num_events = 0; 
  int ntotal_events = 0;

  snprintf(bigbuf, sizeof(bigbuf),"%s/run%d/", config.output_directory, run_number); 
  if (make_dirs_for_output(bigbuf))
  {
      fatal(); 
  }
  output_dir = strdup(bigbuf); 

  if (config.copy_configs) 
  {
    copy_configs(); 
  }


  //Copy any other things we want to the run directory 
  char * thing_to_copy; 
  char * tmp_str = strdup(config.copy_paths_to_rundir); 
  char * save_ptr = 0;
  thing_to_copy = strtok_r(tmp_str,":",&save_ptr); 
  while (thing_to_copy!=NULL)
  {
     snprintf(bigbuf,sizeof(bigbuf), "cp -r %s %s/aux", thing_to_copy, output_dir); 
     system(bigbuf); 
     thing_to_copy = strtok_r(NULL,":",&save_ptr);
  }
  free(tmp_str); 

 
  while(1) 
  {
    time_t now; 
    time(&now); 
    int have_data= 0; 
    int have_status = 0;
    
    size_t occupancy = beacon_buf_occupancy(acq_buffer); 
    if (beacon_buf_occupancy(acq_buffer))
    {
        events = beacon_buf_pop(acq_buffer, events); 
        num_events += events->nfilled; 
        ntotal_events += events->nfilled;
        have_data=1;
    }

    if (beacon_buf_occupancy(mon_buffer)) 
    {
      mon = beacon_buf_pop(mon_buffer, mon); 
      have_status=1; 
    }
    
    //print something to screen
    if (config.print_interval > 0 && now - last_print_out > config.print_interval)  
    {
      printf("---------after %u seconds-----------\n", (unsigned) (now - start_time)); 
      printf("  total events written: %d\n", ntotal_events); 
      printf("  write rate:  %g Hz\n", (num_events == 0) ? 0. :  ((float) num_events) / (now - last_print_out)); 
      printf("  write buffer occupancy: %zu \n", occupancy); 
      if (!config.use_fixed_thresholds) fs_avg_print(stdout); 
      beacon_status_print(stdout, last_status); 
      if (!config.use_fixed_thresholds) pid_state_print(stdout, &last_pid); 
      last_print_out = now; 
      num_events = 0;
    }


    if (!have_data && !have_status)
    {
      if (die) 
      {
        if (data_file)  do_close(data_file,data_file_name); 
        if (header_file)  do_close(header_file, header_file_name); 
        if (status_file)  do_close(status_file, status_file_name); 

        break; 
      }

      //no data, so sleep a lot
      usleep(50000);  //50 ms 
      continue; 
    }
   
    //make sure we have the right dirs
  
        
    if (have_data)
    {
      int j; 

      for (j = 0; j < events->nfilled; j++)
      {

        if (!data_file || data_file_size >= config.events_per_file)
        {
          if (data_file) do_close(data_file, data_file_name); 
          snprintf(bigbuf,sizeof(bigbuf),"%s/run%d/event/%"PRIu64".event.gz%s", config.output_directory,run_number,  events->events[j].event_number, tmp_suffix ); 
          data_file = gzopen(bigbuf,"w");  //TODO add error check
          data_file_name = strdup(bigbuf); 
          data_file_size = 0; 
        }

        if (!header_file || header_file_size >= config.events_per_file)
        {
          if (header_file) do_close(header_file, header_file_name); 
          snprintf(bigbuf,sizeof(bigbuf),"%s/run%d/header/%"PRIu64".header.gz%s", config.output_directory,run_number, events->headers[j].event_number, tmp_suffix ); 
          header_file = gzopen(bigbuf,"w");  //TODO add error check
          header_file_name = strdup(bigbuf); 
          header_file_size = 0; 
        }
       
        beacon_event_gzwrite(data_file, &events->events[j]); 
        beacon_header_gzwrite(header_file, &events->headers[j]); 
        data_file_size++; 
        header_file_size++; 
      }
    }

    if (have_status)
    {
      if (!status_file || status_file_size >= config.status_per_file)
      {
        if (status_file) do_close(status_file, status_file_name); 
        snprintf(bigbuf,sizeof(bigbuf),"%s/run%d/status/%u.status.gz%s", config.output_directory, run_number,  (unsigned) now, tmp_suffix); 
        status_file = gzopen(bigbuf,"w");  //TODO add error check
        status_file_name = strdup(bigbuf); 
        status_file_size = 0; 
      }

      memcpy(last_status, &mon->status, sizeof(*last_status)); 
      if (!config.use_fixed_thresholds) memcpy(&last_pid, &mon->control, sizeof(last_pid)); 

      //update the mmaped file if necessary 
      if ( saved_status == last_status) msync(saved_status, sizeof(beacon_status_t),MS_ASYNC); 


      //write out the file 
      beacon_status_gzwrite(status_file, &mon->status); 

      status_file_size++; 
    }
    
    //had data, so sleep just a little (unless occupancy is too high) 
    if (beacon_buf_occupancy(acq_buffer) < config.buffer_capacity/3)  usleep(25000);  //25 ms 
  }

  if (last_status != saved_status)  free(last_status); 

  return 0; 

}


void fatal()
{
  die = 1; 

  //cancel any waits 
  if (device) 
    beacon_cancel_wait(device); 

}

void signal_handler(int signal, siginfo_t * sig, void * v) 
{
  switch (signal)
  {
    case SIGUSR1: 
      read_config(0); 
      break; 
    case SIGTERM: 
    case SIGUSR2: 
    case SIGINT: 
    default: 
      fprintf(stderr,"Caught deadly signal %d\n", signal); 
      fatal(); 
  }
}

const char * tmp_run_file = "/tmp/.runfile"; 


/* This is to avoid repeating code
 * twice for device things that may be changed on a reread */ 
static int configure_device() 
{
  beacon_set_spi_clock(device, config.spi_clock); 
  beacon_set_buffer_length(device, config.waveform_length); 

  //setup the external trigger
  beacon_trigger_output_config_t trigo; 
  beacon_get_trigger_output(device,&trigo); 
  trigo.enable = config.enable_trigout; 
  trigo.width = config.trigout_width; 
  beacon_configure_trigger_output(device,trigo); 

  beacon_ext_input_config_t trigi; 
  //beacon_get_ext_trigger_in(device,&trigi);// nothing to preserve, so don't bother! 
  trigi.use_as_trigger = config.enable_extin; 
  trigi.trig_delay = round(config.extin_trig_delay_us*1e3/128.); 
  beacon_configure_ext_trigger_in(device,trigi); 


  //set up the calpulser
  beacon_calpulse(device,config.calpulser_state); 

  //set up the pretrigger
  beacon_set_pretrigger(device, (uint8_t) config.pretrigger & 0xf);

  //set up the trigger delays 
  beacon_set_trigger_delays(device, config.trig_delays);

  //set the trigger polarization
  beacon_set_trigger_polarization(device, config.trigger_polarization);

  /* //enable the trigger, if desired */
  /* beacon_set_phased_trigger(device, config.enable_phased_trigger); */
  

  //Set the veto options
  beacon_set_veto_options(device, &config.veto); 

  if (config.apply_attenuations)
  {
    beacon_set_attenuation(device, config.attenuation, 0); 
  }

  beacon_set_trigger_mask(device, config.trigger_mask); 
  beacon_set_channel_mask(device, config.channel_mask); 

  beacon_set_poll_interval(device, config.poll_usecs); 

  beacon_set_trigger_path_low_pass(device, config.enable_low_pass_to_trigger); 
  beacon_set_dynamic_masking(device, config.enable_dynamic_masking, config.dynamic_masking_threshold, config.dynamic_masking_holdoff); 

 
  return 0; 

}

static int setup()
{
  //let's do signal handlers. 
  //My understanding is that the main thread gets all the signals it doesn't block first, so we'll just handle it that way. 

  sigset_t empty;
  sigemptyset(&empty); 
  struct sigaction sa; 
  sa.sa_mask = empty; 
  sa.sa_flags = 0; 
  sa.sa_sigaction = signal_handler; 

  sigaction(SIGINT,  &sa,0); 
  sigaction(SIGTERM, &sa,0); 
  sigaction(SIGUSR1, &sa,0); 
  sigaction(SIGUSR2, &sa,0); 

  //Read configuration 
  int config_return = read_config(1); 

  if (config_return == SETUP_TRY_AGAIN_LATER) 
  {

    return SETUP_TRY_AGAIN_LATER;

  }
  /* open up the run number file, read the run, then increment it and save it
   * This is a bit fragile, potentially, so maybe revisit. 
   * */ 
  FILE * run_file = fopen(config.run_file, "r"); 
  fscanf(run_file, "%d\n", &run_number); 
  fclose(run_file); 

  run_file = fopen(tmp_run_file,"w"); 
  fprintf(run_file,"%d\n", run_number+1); 
  fclose(run_file); 
  rename(tmp_run_file, config.run_file); 

  //run the reconfiguration / alignment program, if necessary 
  // In the future, this might be replaced by a less hacky way of doing this 
  if (config.alignment_command) 
  {
    printf("Running: %s\n", config.alignment_command); 
    int success = system(config.alignment_command); 
    while (!success) 
    {
      fprintf(stderr,"Alignment not successful. Trying a reset.\n"); 
      //try to do a restart and try again
      beacon_reboot_fpga_power(1,20); 

      if (start_config.reconfigure_fpga_cmd)
      {
        printf("Reconfiguring FPGA's"); 
        system(start_config.reconfigure_fpga_cmd); 
      }

      success = system(config.alignment_command); 

      if (success && !config.apply_attenuations)
      {
        char cmd[1024]; 
        sprintf(cmd,"%s %g", start_config.set_attenuation_cmd, start_config.desired_rms); 
        system(cmd); 
      }
    }

//    printf("Sleeping for 10 seconds\n"); 
//    sleep(10); 

  }


  //open the devices and configure properly
  // the gpio state should already have been set 
  device = beacon_open(config.spi_device, 0, 0, 1); 


  if (!device)
  {
    //that would not be really good 
    fprintf(stderr,"Couldn't open device. Aborting! \n"); 
    exit(1); 
  }

  //If we are loading the thresholds from the status file,
  //we'll mmap the file and copy thresholds over there. 
  if (config.load_thresholds_from_status_file) 
  {

    status_save_fd = open(config.status_save_file, O_CREAT | O_RDWR,00755); 

    if (status_save_fd == -1) 
    {
      fprintf(stderr,"Could not open %s\n", config.status_save_file); 
    }
    else
    {

      //seek to the end to determine the size
      size_t file_size = lseek(status_save_fd, 0, SEEK_END); 

      //rewind the file 
      lseek(status_save_fd,0,SEEK_SET); 


      // If it's not the right size (either 0, or maybe an old version of the struct,
      // truncate it) 
      if (file_size != sizeof(beacon_status_t))
      {
        ftruncate(status_save_fd, sizeof(beacon_status_t)); 
      }

      //mmap it to save_status
      saved_status = mmap(0, sizeof(beacon_status_t), PROT_READ | PROT_WRITE, MAP_SHARED, status_save_fd, 0); 

      // if successful and right size, set the thresholds 
      if (saved_status!=MAP_FAILED && file_size == sizeof(beacon_status_t))
      {
        beacon_set_thresholds(device, saved_status->trigger_thresholds, 0); 
      }
    }
  }

  uint64_t run64 = run_number; 

  //Set event number offset
  beacon_set_readout_number_offset(device, run64 * 1000000000); 

  configure_device(); 

  //set up the beamforming trigger 
  //Right now, this will just always be on. 
  /* beacon_trigger_enable_t slave_enables = beacon_get_trigger_enables(device,SLAVE);  */
  /* slave_enables.enable_beamforming = 1;  */
  /* beacon_set_trigger_enables(device, slave_enables, SLAVE);  */

  beacon_trigger_enable_t master_enables = beacon_get_trigger_enables(device,MASTER); 
  master_enables.enable_beamforming = 1;
  beacon_set_trigger_enables(device, master_enables, MASTER); 
 

  // set up the buffers
  acq_buffer = beacon_buf_init( config.buffer_capacity, sizeof(acq_buffer_t)); 
  mon_buffer = beacon_buf_init( config.buffer_capacity, sizeof(monitor_buffer_t)); 


  // set up the threads 
 
  pthread_create(&the_mon_thread, 0, monitor_thread, 0); 
  pthread_create(&the_acq_thread, 0, acq_thread, 0); 
  pthread_create(&the_wri_thread, 0, write_thread, 0); 
  

  //increase priority of acquistion thread
  if (config.realtime_priority > 0) 
  {
    struct sched_param sp;
    sp.sched_priority = config.realtime_priority; 
    pthread_setschedparam(the_acq_thread, SCHED_FIFO, &sp); 
  }


  return 0;
}

int teardown() 
{
  pthread_join(the_acq_thread,0); 
  pthread_join(the_mon_thread,0); 
  pthread_join(the_wri_thread,0); 

  //Turn off calpulser 
  beacon_calpulse(device,0); 

  //optionally, turn off the phased trigger output 
  beacon_trigger_output_config_t trigo; 
  beacon_get_trigger_output(device,&trigo); 
  trigo.enable = trigo.enable && !config.disable_trigout_on_exit; 
  beacon_configure_trigger_output(device,trigo); 


  beacon_close(device); 


  //munmap the persistent status if necessary 
  if (saved_status != 0 && saved_status != MAP_FAILED) 
  {
    munmap (saved_status, sizeof(beacon_status_t));
    close(status_save_fd); 
  }

  return 0;
}


int read_config(int first_time)
{

  char * cfgpath = 0;  
  char * start_cfgpath = 0;  
  char * hk_cfgpath = 0;  
  

  pthread_mutex_lock(&config_lock); 

  if (first_time)
  {
    beacon_acq_config_init(&config); 
    beacon_hk_config_init(&hk_config); 
    beacon_start_config_init(&start_config); 
  }

  if (!beacon_get_cfg_file(&cfgpath, BEACON_ACQ))
  {
    printf("Using config file: %s\n", cfgpath); 
  }

   if (!beacon_get_cfg_file(&start_cfgpath, BEACON_STARTUP))
  {
    printf("Using startup config file: %s\n", start_cfgpath); 
  }

 
   if (!beacon_get_cfg_file(&hk_cfgpath, BEACON_HK))
  {
    printf("Using hk config file: %s\n", hk_cfgpath); 
  }
  

  beacon_acq_config_read( cfgpath, &config); 
  beacon_start_config_read(start_cfgpath, &start_config); 
  beacon_hk_config_read(hk_cfgpath, &hk_config); 

  pthread_mutex_unlock(&config_lock); 

  //open the shared hk
  if (first_time) 
  {
    int opened = open_shared_hk(&hk_config, 1, &the_hk); 
    if (opened) 
    {
      fprintf(stderr, "Could not open shared hk... aborting\n"); 
      return SETUP_FAILED; 
    }
  }


  if (first_time && (config.check_power_on || config.auto_power_on || config.auto_power_off))
  {
    int quick_exit = 0;
    //check if the adc current is above
    lock_shared_hk(); 
    if (the_hk->adc_current < config.adc_threshold_for_on)
    {
      quick_exit = 1; 

      if (config.auto_power_on && the_hk->cc_batt_dV*10 > config.cc_voltage_to_turn_on && the_hk->inv_batt_dV*10 > config.inv_voltage_to_turn_on) 
      {
        fprintf(stderr," Voltages exceeded power on threshold. Will turn on...\n"); 
        system(config.power_on_command); 
      }
      else
      {
        fprintf(stderr, "adc current is %d, below threshold of %d. Either the ADC is not on or beacon-hk is not running... Exiting.\n", the_hk->adc_current, config.adc_threshold_for_on); 
      }
    }
    if (the_hk->disk_space_kB < 1e6) 
    {
      fprintf(stderr, "Disk space is only %f GB. Will try again later.\n", the_hk->disk_space_kB/1e6); 
      quick_exit = 1; 
    }
    unlock_shared_hk(); 
    if (quick_exit) 
    {
      return SETUP_TRY_AGAIN_LATER; 
    }
  }



  ///ok, if we got this far, we'll actually start things up 


  pid_state_init(&control, config.k_p, config.k_i, config.k_d); 

  if (first_time)
  {
    fs_avg_init(config.n_fast_scaler_avg); 
  }

  if (!first_time) 
  {

    configure_device(); 

    //rewrite run number in case we are using a different file 
    FILE * run_file = fopen(tmp_run_file,"w"); 
    fprintf(run_file,"%d\n", run_number+1); 
    fclose(run_file); 
    rename(tmp_run_file, config.run_file); 
  }


  if (!first_time && config.copy_configs)
  {
    copy_configs(); 
  }


  free(cfgpath); 
  free(start_cfgpath); 

  return 0;
}

