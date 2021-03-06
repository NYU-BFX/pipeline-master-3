//
// Copyright (c) 2014 Aristotelis Tsirigos
// All rights reserved. This program and the accompanying materials are made available under the terms of the GNU General Public License v2.0 
// which accompanies this distribution, and is available at http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <gsl/gsl_rng.h>
#include <gsl/gsl_cdf.h>
#include <gsl/gsl_randist.h>
#include <gsl/gsl_permutation.h>
#include <map>
#include <string>
#include <list>
#include <queue>
#include <vector>
#include <iostream>
#include <algorithm>
#include "core.h"
#include "gtools-intervals.h"


using namespace std;


//---------------------------------------------------------------------------------//
// Global variables                                                                //
//---------------------------------------------------------------------------------//

const string PROGRAM = "gtools-scans";
const long int BUFFER_SIZE = 10000;
gsl_rng *RANDOM_GENERATOR;



  
//---------------------------------------------------------------------------------//
// Command-line options                                                            //
//---------------------------------------------------------------------------------//

bool VERBOSE;
bool HELP;
bool SORTED;
bool REF_SORTED;
char *GENOME_REG_FILE = NULL;
char *REF_REG_FILE = NULL;
long int MIN_READS;
bool IGNORE_STRAND;
long int WIN_DIST;
long int WIN_SIZE;
char PREPROCESS;
bool NORM;
bool COMPARE;
long int MAX_LABEL_VALUE;
char *METHOD;
double PVAL_CUTOFF;
double QVAL_CUTOFF;
bool PRINT_DETAILS;





//-------InitCmdLine-----------
//
CmdLineWithOperations *InitCmdLine(int argc, char *argv[], int *next_arg)
{
  // initialize
  CmdLineWithOperations *cmd_line = new CmdLineWithOperations(); 
  cmd_line->SetProgramName(PROGRAM,VERSION);

  // set operations
  cmd_line->AddOperation("counts", "[OPTIONS] <REG-FILE>", \
  "Determines input read counts in sliding windows of reference regions.", \
  "* Input formats: REG, GFF, BED, SAM\n\
  * Operand: interval\n\
  * Region requirements: single-interval if -S option is set\n\
  * Region-set requirements: sorted by chromosome/strand/start if -S option is set"\
  );

  cmd_line->AddOperation("peaks", "[OPTIONS] SIGNAL-REG-FILE [CONTROL-REG-FILE [GENOME-UNIQ-REG-FILE]]", \
  "Scans input reads to identify peaks.", \
  "* Input formats: REG, GFF, BED, SAM\n\
  * Operand: interval\n\
  * Region requirements: single-interval if -S option is set\n\
  * Region-set requirements: sorted by chromosome/strand/start if -S option is set"\
  );

  if (argc<2) { cmd_line->OperationSummary("OPERATION [OPTIONS] INPUT-FILES","Performs whole-genome scanning operations."); exit(1); }

  // current operation
  string op = argv[1];
  if (op[0]=='-') op = op.substr(1);		// ensure compatibility with previous version
  cmd_line->SetCurrentOperation(op);
  
  // common options
  cmd_line->AddOption("--help", &HELP, false, "help");
  cmd_line->AddOption("-h", &HELP, false, "help");
  cmd_line->AddOption("-v", &VERBOSE, false, "verbose mode");
  
  // Main options
  int n_args;
  if (cmd_line->current_cmd_operation=="counts") {
    n_args = 0;
    cmd_line->AddOption("-S", &SORTED, false, "input regions are sorted");
    cmd_line->AddOption("-Sref", &REF_SORTED, false, "reference regions (option -r) are sorted");
    cmd_line->AddOption("-g", &GENOME_REG_FILE, "", "genome region file");
    cmd_line->AddOption("-r", &REF_REG_FILE, "", "reference region file");
    cmd_line->AddOption("-i", &IGNORE_STRAND, false, "ignore strand information");
    cmd_line->AddOption("-op", &PREPROCESS, '1', "preprocess operator (1=start, c=center, p=all points)");
    cmd_line->AddOption("--max-label-value", &MAX_LABEL_VALUE, 1, "maximum region label value to be used");
    cmd_line->AddOption("-w", &WIN_SIZE, 500, "window size (must be a multiple of window distance)");
    cmd_line->AddOption("-d", &WIN_DIST, 25, "window distance");
    cmd_line->AddOption("-min", &MIN_READS, 10, "minimum reads in window");
  }
  else if (cmd_line->current_cmd_operation=="peaks") {
    n_args = 1;
    cmd_line->AddOption("-S", &SORTED, false, "input regions are sorted");
    cmd_line->AddOption("-g", &GENOME_REG_FILE, "genome.reg+", "genome region file");
    cmd_line->AddOption("-i", &IGNORE_STRAND, false, "ignore strand information");
    cmd_line->AddOption("--max-label-value", &MAX_LABEL_VALUE, 1, "maximum region label value to be used");
    cmd_line->AddOption("-w", &WIN_SIZE, 500, "window size (must be a multiple of window distance)");
    cmd_line->AddOption("-d", &WIN_DIST, 25, "window distance");
    cmd_line->AddOption("-min", &MIN_READS, 10, "minimum reads in window");
    cmd_line->AddOption("-M", &METHOD, "binomial", "method (binomial, poisson)");
    cmd_line->AddOption("-norm", &NORM, false, "equalize background probabilities");
    cmd_line->AddOption("-cmp", &COMPARE, false, "compare signal to control window");
    cmd_line->AddOption("-pval", &PVAL_CUTOFF, 1.0, "pvalue cutoff");
    cmd_line->AddOption("-qval", &QVAL_CUTOFF, 0.05, "qvalue cutoff");
    cmd_line->AddOption("-D", &PRINT_DETAILS, false, "print details");
  }
  else {
    cerr << "Unknown operation '" << op << "'!\n";
    delete cmd_line;
    exit(1);
  }

  // process command line
  *next_arg = cmd_line->Read(argv+1,argc-1) + 1;
  if (HELP||(argc-*next_arg<n_args)) { cmd_line->OperationUsage(); delete cmd_line; exit(1); }
  
  return cmd_line;
}



//-----Max_----------
//
double Max_(double x, double y)
{
  return x>y?x:y;
}


//-----ComputeQValues----------
//
double ComputeQValues(list<double> pval, list<double>pval_rnd, long int n_permutations, double qval_cutoff)
{
  // sort p-values
  list<double> pval_sorted = pval;
  list<double> pval_rnd_sorted = pval_rnd;
  pval_sorted.sort();
  pval_rnd_sorted.sort();
  
  // scan random p-values  
  long int n = pval_sorted.size();
  unsigned long int *counts = new unsigned long int[n];
  for (long int k=0; k<n; k++) counts[k] = 0;
  long int k = 0;
  for (list<double>::iterator i=pval_sorted.begin(),j=pval_rnd_sorted.begin(); (i!=pval_sorted.end())&&(j!=pval_rnd_sorted.end()); j++) {
    while ((i!=pval_sorted.end())&&(*j>*i)) { i++; k++; }
    if (k<n-1) counts[k]++;
  }

  // compute q-values
  double *q_sorted = new double[n];
  for (long int k=0; k<n; k++) {
    q_sorted[k] = (float)counts[k]/n_permutations/(k+1);
    if (k+1==n) break;
    counts[k+1] += counts[k];
  }
  //printf("counts = ["); for (long int k=0; k<n; k++) printf(" %ld", counts[k]); printf(" ]\n");
  //printf("q_sorted = ["); for (long int k=0; k<n; k++) printf(" %.4e", q_sorted[k]); printf(" ]\n");

  // correct q-values
  float min_q = q_sorted[n-1];
  list<double>::reverse_iterator p = pval_sorted.rbegin();
  double pval_cutoff = -1.0;
  for (long int k=n-2; k>=0; k--,p++) {
    if (min_q<=qval_cutoff) { pval_cutoff = *p; break; }
    if (q_sorted[k]>min_q) q_sorted[k] = min_q;
    else min_q = q_sorted[k];
  }
    
  // cleanup
  delete counts;
  delete q_sorted;
  
  return pval_cutoff;
}



//---------------------------------------------------------------------------------------------------------------------------//
// CLASS: PeakFinder                                                                                                         //
//---------------------------------------------------------------------------------------------------------------------------//

class PeakFinder
{
 public:
  PeakFinder(char *signal_reg_file, char *control_reg_file, char *uniq_reg_file, char *genome_reg_file);
  ~PeakFinder();
  
  void Run();
  void Reset();

  StringLIntMap *bounds;
  unsigned long int effective_genome_size;
  long int n_signal_reads, n_control_reads;
  GenomicRegionSet *SignalRegSet, *ControlRegSet, *UniqRegSet;
  double p_signal, p_control, p_ratio;
  GenomicRegionSetScanner *signal_scanner, *control_scanner, *uniq_scanner;
};



//-----Constructor----------
//
PeakFinder::PeakFinder(char *signal_reg_file, char *control_reg_file, char *uniq_reg_file, char *genome_reg_file)
{
  char preprocess = SORTED?'1':'c';
  if (uniq_reg_file!=NULL) preprocess = '1';

  bounds = ReadBounds(genome_reg_file);
  effective_genome_size = uniq_reg_file==NULL?CalcBoundSize(bounds):CalcRegSize(uniq_reg_file);
  fprintf(stderr, "* Effective genome size = %lu\n", effective_genome_size);
    
  n_signal_reads = CountGenomicRegions(signal_reg_file,MAX_LABEL_VALUE);
  SignalRegSet = new GenomicRegionSet(signal_reg_file,BUFFER_SIZE,VERBOSE,false,true);
  p_signal = (double)n_signal_reads/effective_genome_size;
  if (SORTED) signal_scanner = new SortedGenomicRegionSetScanner(SignalRegSet,bounds,WIN_DIST,WIN_SIZE,MAX_LABEL_VALUE,IGNORE_STRAND,preprocess);
  else signal_scanner = new UnsortedGenomicRegionSetScanner(SignalRegSet,bounds,WIN_DIST,WIN_SIZE,MAX_LABEL_VALUE,IGNORE_STRAND,preprocess);

  if (control_reg_file!=NULL) {
    n_control_reads = CountGenomicRegions(control_reg_file,MAX_LABEL_VALUE);
    ControlRegSet = new GenomicRegionSet(control_reg_file,BUFFER_SIZE,VERBOSE,false,true);
    p_control = (double)n_control_reads/effective_genome_size;
    if (SORTED) control_scanner = new SortedGenomicRegionSetScanner(ControlRegSet,bounds,WIN_DIST,WIN_SIZE,MAX_LABEL_VALUE,IGNORE_STRAND,preprocess);
    else control_scanner = new UnsortedGenomicRegionSetScanner(ControlRegSet,bounds,WIN_DIST,WIN_SIZE,MAX_LABEL_VALUE,IGNORE_STRAND,preprocess);
  }
  else {
    ControlRegSet = NULL;
    n_control_reads = n_signal_reads;
    p_control = p_signal;
    control_scanner = NULL;
  }

  p_ratio = p_signal/p_control;
  fprintf(stderr, "* Signal input file = %s (reads = %lu; background probability = %.2e)\n", signal_reg_file, n_signal_reads, p_signal);
  fprintf(stderr, "* Control input file = %s (reads = %lu; background probability = %.2e)\n", control_reg_file, n_control_reads, p_control);
  fprintf(stderr, "* Signal/Control background probability = %f\n", p_ratio);

  // NOTE: UniqRegSet must be sorted, because preprocess option 'p' is not implemented for unsorted region scanners
  UniqRegSet = uniq_reg_file==NULL?NULL:new GenomicRegionSet(uniq_reg_file,BUFFER_SIZE,VERBOSE,false,true);
  uniq_scanner = uniq_reg_file==NULL?NULL:new SortedGenomicRegionSetScanner(UniqRegSet,bounds,WIN_DIST,WIN_SIZE,1,IGNORE_STRAND,'p');
}



    
//-----Destructor----------
//
PeakFinder::~PeakFinder()
{
  if (bounds!=NULL) delete bounds;
  if (SignalRegSet!=NULL) delete SignalRegSet;
  if (signal_scanner!=NULL) delete signal_scanner;
  if (ControlRegSet!=NULL) delete ControlRegSet;
  if (control_scanner!=NULL) delete control_scanner;
  if (UniqRegSet!=NULL) delete UniqRegSet;
  if (uniq_scanner!=NULL) delete uniq_scanner;
}



//-----Run----------
//
void PeakFinder::Run()
{
  list<double> pval1_list, pval2_list;
  list<GenomicInterval*> interval_list;
  
  long int v1, v2, v0;
  Progress PRG("Scanning...",1);
  while (((v1=signal_scanner->Next())!=-1)) {
    v2 = control_scanner?control_scanner->Next():gsl_ran_poisson(RANDOM_GENERATOR,WIN_SIZE*p_signal);
    v0 = uniq_scanner==NULL?WIN_SIZE:uniq_scanner->Next(); 
    v1 = min(v1,v0);
    v2 = min(v2,v0);

    if (NORM) { if (p_ratio<1.0) v2 = (long int)floor((float)v2*p_ratio); else v1 = (long int)floor((float)v1/p_ratio); }

    // statistical test
    if (v1>=MIN_READS) {
      double pval1, pval2;
      if (COMPARE) {
        if (strcmp(METHOD,"binomial")==0) { 
          float pp_control = ((float)v2+1.0)/(v0+1.0);
          pval1 = (double)gsl_cdf_binomial_Q(v1,Max_(pp_control,p_signal),v0+1);
          float pp_signal = ((float)v1+1.0)/(v0+1.0);
          pval2 = (double)gsl_cdf_binomial_Q(v2,Max_(pp_signal,p_control),v0+1);
        }
        else if (strcmp(METHOD,"poisson")==0) {
          long int pseudo = 5;
          pval1 = (double)gsl_cdf_poisson_Q(v1+pseudo,v2+pseudo);
          pval2 = (double)gsl_cdf_poisson_Q(v2+pseudo,v1+pseudo);
        }
        else if (strcmp(METHOD,"binomial2")==0) {
          double pp_control = (double)(v2+1)/n_control_reads;
          double pp_signal = (double)(v1+1)/n_signal_reads;
          pval1 = (double)gsl_cdf_binomial_Q(v1+1,pp_control,n_signal_reads);
          pval2 = (double)gsl_cdf_binomial_Q(v2+1,pp_signal,n_control_reads);
        }
        else if (strcmp(METHOD,"cbinomial")==0) {
          pval1 = (double)gsl_cdf_binomial_Q(v1+1,0.5,v1+v2+2);
          pval2 = (double)gsl_cdf_binomial_Q(v2+1,0.5,v1+v2+2);
        }
        else if (strcmp(METHOD,"normal")==0) {
          double pp_control = (double)(v2+1)/n_control_reads;
          double pp_signal = (double)(v1+1)/n_signal_reads;
          pval1 = (double)gsl_cdf_ugaussian_Q((v1+1-n_signal_reads*pp_control)/sqrt(n_signal_reads*pp_control));
          pval2 = (double)gsl_cdf_ugaussian_Q((v2+1-n_control_reads*pp_signal)/sqrt(n_control_reads*pp_signal));
        }
        else { cerr << "Error: unknown probability distribution!\n"; exit(1); }
      }
      else {
        if (strcmp(METHOD,"binomial")==0) { 
          pval1 = (double)gsl_cdf_binomial_Q(v1,p_signal,v0+1);
          pval2 = (double)gsl_cdf_binomial_Q(v2,p_control,v0+1);
        }
        else if (strcmp(METHOD,"poisson")==0) {
          long int pseudo = 5;
          pval1 = (double)gsl_cdf_poisson_Q(v1+pseudo,v2+pseudo);
          pval2 = (double)gsl_cdf_poisson_Q(v2+pseudo,v1+pseudo);
        }
        else { cerr << "Error: unknown probability distribution!\n"; exit(1); }
      }

      if (pval1<=PVAL_CUTOFF) {
	    interval_list.push_back(signal_scanner->GetInterval());
		pval1_list.push_back(pval1);
		pval2_list.push_back(pval2);
      }

    }
    PRG.Check();
  }
  PRG.Done();

  double pval_cutoff = ComputeQValues(pval1_list,pval2_list,1,QVAL_CUTOFF);
  long int n = interval_list.size();
  Progress PRG2("Printing...",n);
  list<GenomicInterval*>::iterator g = interval_list.begin(); 
  list<double>::iterator p = pval1_list.begin();
  list<double>::iterator prnd = pval2_list.begin();
  for (long int k=0; k<n; k++,g++,p++,prnd++) {
    //if (PRINT_DETAILS) printf("%.4e|%.4e|%.4e\t", *p, *prnd, qval[k]);
    if (*p<=pval_cutoff) { printf("%.4e\t", *p); (*g)->Print(); }
	PRG.Check();
  }
  PRG.Done();
  interval_list.clear();
}


//---------------------------------------------------------------------------------------------------------------------------//
// END CLASS: PeakFinder                                                                                                     //
//---------------------------------------------------------------------------------------------------------------------------//











//-----RunCounts----------
//
void RunCounts(char *input_reg_file, char *ref_reg_file, char *genome_reg_file, bool sorted)
{
  // initialize
  StringLIntMap *bounds = ReadBounds(genome_reg_file);
  GenomicRegionSet InputRegSet(input_reg_file,BUFFER_SIZE,VERBOSE,false,true);
  GenomicRegionSetScanner *input_scanner;
  if (sorted) input_scanner = new SortedGenomicRegionSetScanner(&InputRegSet,bounds,WIN_DIST,WIN_SIZE,MAX_LABEL_VALUE,IGNORE_STRAND,PREPROCESS);
  else input_scanner = new UnsortedGenomicRegionSetScanner(&InputRegSet,bounds,WIN_DIST,WIN_SIZE,MAX_LABEL_VALUE,IGNORE_STRAND,PREPROCESS);

  // setup reference regions
  GenomicRegionSet *RefRegSet = NULL;
  GenomicRegionSetIndex *RefIndex = NULL;
  if (strlen(ref_reg_file)>0) {
	  if (REF_SORTED) RefRegSet = new GenomicRegionSet(ref_reg_file,BUFFER_SIZE,VERBOSE,false,true);
	  else {
		  RefRegSet = strlen(ref_reg_file)>0?new GenomicRegionSet(ref_reg_file,BUFFER_SIZE,VERBOSE,true,true):NULL;
		  RefIndex = new GenomicRegionSetIndex(RefRegSet,"17,20,23,26");
	  }
  }

  // run
  Progress PRG("Scanning...",1);
  for (long int v=REF_SORTED?input_scanner->Next(RefRegSet):input_scanner->Next(RefIndex); v!=-1; v=REF_SORTED?input_scanner->Next(RefRegSet):input_scanner->Next(RefIndex)) {
    if (v>=MIN_READS) {
      cout << v << '\t';
      input_scanner->PrintInterval();
      cout << '\n';
    }
    PRG.Check();
  }
  PRG.Done();
  
  // cleanup
  delete input_scanner;
  if (bounds!=NULL) delete bounds;
  if (RefRegSet!=NULL) delete RefRegSet;
  if (RefIndex!=NULL) delete RefIndex;
}









//---------------------------------------------------------------------------------//
// MAIN	                                                                           //
//---------------------------------------------------------------------------------//
int main(int argc, char* argv[]) 
{
  // process command-line arguments
  int next_arg;
  CmdLineWithOperations *cmd_line = InitCmdLine(argc,argv,&next_arg); 
  _MESSAGES_ = VERBOSE;
  
  // initilize random generator
  RANDOM_GENERATOR = InitRandomGenerator(getpid()+time(NULL));

  if (cmd_line->current_cmd_operation=="counts") {
    char *INPUT_REG_FILE = next_arg==argc ? NULL : argv[next_arg];
    RunCounts(INPUT_REG_FILE,REF_REG_FILE,GENOME_REG_FILE,SORTED);
  }
  
  else if (cmd_line->current_cmd_operation=="peaks") {
    char *SIGNAL_REG_FILE = next_arg==argc ? NULL : argv[next_arg];
    char *CONTROL_REG_FILE = next_arg+1<argc?argv[next_arg+1]:NULL; 
    char *UNIQ_REG_FILE = next_arg+2<argc?argv[next_arg+2]:NULL;
    PeakFinder peak_finder(SIGNAL_REG_FILE,CONTROL_REG_FILE,UNIQ_REG_FILE,GENOME_REG_FILE);
    peak_finder.Run();
  }
  
  // clean up
  delete cmd_line;
  delete RANDOM_GENERATOR;
  
  return 0;
}




