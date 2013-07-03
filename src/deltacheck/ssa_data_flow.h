/*******************************************************************\

Module: Data Flow Analysis

Author: Daniel Kroening, kroening@kroening.com

\*******************************************************************/

#ifndef DELTACHECK_SSA_DATA_FLOW_H
#define DELTACHECK_SSA_DATA_FLOW_H

#include <util/threeval.h>

#include "function_ssa.h"
#include "solver.h"
#include "predicate.h"

class ssa_data_flowt
{
public:
  typedef function_SSAt::locationt locationt;

  explicit ssa_data_flowt(const function_SSAt &_function_SSA):
    function_SSA(_function_SSA)
  {
    fixed_point();
  }

  void print_invariant(std::ostream &) const;
  
  unsigned iteration_number;
  
  class assertiont
  {
  public:
    locationt loc;
    tvt status;
    exprt guard;
  };
  
  typedef std::list<assertiont> assertionst;
  assertionst assertions;
  
protected:
  const function_SSAt &function_SSA;
  
  void fixed_point();
  bool iteration();
  void initialize_invariant();

  struct backwards_edget
  {
    locationt from, to;
    predicatet pre_predicate, post_predicate;
  };
  
  backwards_edget backwards_edge(locationt loc);
  
  typedef std::vector<backwards_edget> backwards_edgest;
  backwards_edgest backwards_edges;
  void get_backwards_edges();

  void check_assertions(solvert &solver);
  void setup_assertions();
};

#endif