/*******************************************************************\

Module: Summarizer

Author: Peter Schrammel

\*******************************************************************/

#include <iostream>

#include <util/simplify_expr.h>
#include <solvers/sat/satcheck.h>
#include <solvers/flattening/bv_pointers.h>
#include <solvers/smt2/smt2_dec.h>
#include <util/find_symbols.h>

#include "summarizer.h"
#include "summary_db.h"

#include "../domains/ssa_analyzer.h"
#include "../domains/template_generator_summary.h"
#include "../domains/template_generator_callingcontext.h"
#include "../domains/template_generator_ranking.h"

#include "../ssa/local_ssa.h"
#include "../ssa/simplify_ssa.h"

#define PRECISE_JOIN


/*******************************************************************\

Function: summarizert::initialize_preconditions()

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

void summarizert::initialize_preconditions(functionst &_functions, 
					   bool forward, 
					   bool sufficient)
{
  preconditions.clear();
  for(functionst::const_iterator it = _functions.begin(); 
      it!=_functions.end(); it++)
  {
    preconditions[it->first] = true_exprt();
  }
}

/*******************************************************************\

Function: summarizert::summarize()

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

void summarizert::summarize(bool forward, bool sufficient)
{
  initialize_preconditions(ssa_db.functions(),forward,sufficient);
  for(functionst::const_iterator it = ssa_db.functions().begin(); 
      it!=ssa_db.functions().end(); it++)
  {
    status() << "\nSummarizing function " << it->first << eom;
    if(!summary_db.exists(it->first)) 
      compute_summary_rec(it->first,false,forward,sufficient);
    else status() << "Summary for function " << it->first << 
           " exists already" << eom;
  }
}

/*******************************************************************\

Function: summarizert::summarize()

  Inputs:

 Outputs:

 Purpose: summarize from given entry point

\*******************************************************************/

void summarizert::summarize(const function_namet &function_name,
                            bool forward, bool sufficient)
{
  initialize_preconditions(ssa_db.functions(),forward,sufficient);

  status() << "\nSummarizing function " << function_name << eom;
  if(!summary_db.exists(function_name)) 
  {
    compute_summary_rec(function_name,true,forward,sufficient);
  }
  else status() << "Summary for function " << function_name << 
	 " exists already" << eom;

  //adding preconditions to SSA for assertion check
  for(functionst::const_iterator it = ssa_db.functions().begin(); 
      it!=ssa_db.functions().end(); it++)
  {
    if(it->second->nodes.empty()) continue;
    it->second->nodes.front().constraints.push_back(preconditions[it->first]);
  }
}

/*******************************************************************\

Function: summarizert::compute_summary_rec()

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

void summarizert::compute_summary_rec(const function_namet &function_name,
				      bool context_sensitive,
				      bool forward,
				      bool sufficient)
{
  local_SSAt &SSA = ssa_db.get(function_name); 
  
  bool calls_terminate = true;
  bool has_function_calls = false;  
  exprt::operandst postconditions;
  // recursively compute summaries for function calls
  inline_summaries(function_name,SSA,context_sensitive,forward,sufficient,
		   calls_terminate, has_function_calls,postconditions); 

  status() << "Analyzing function "  << function_name << eom;

  {
    std::ostringstream out;
    out << "Function body for " << function_name << 
      " to be analyzed: " << std::endl;
    for(local_SSAt::nodest::iterator n = SSA.nodes.begin(); 
        n!=SSA.nodes.end(); n++)
    {
      if(!n->empty()) n->output(out,SSA.ns);
    }
    out << "(enable) " << from_expr(SSA.ns, "", SSA.get_enabling_exprs()) << "\n";
    debug() << out.str() << eom;
  }

  bool has_loops = false;  
  if(options.get_bool_option("termination"))
  {
    for(local_SSAt::nodest::iterator n_it = SSA.nodes.begin(); 
        n_it!=SSA.nodes.end(); n_it++)
    {
      if(n_it->loophead != SSA.nodes.end()) { has_loops = true; break; }
    }
  }

  if(options.get_bool_option("termination"))
  {
    debug() << "function " << 
      (has_function_calls ? "has function calls" : "does not have function calls") << eom;
    debug() << "function calls " << 
      (calls_terminate ? "terminate" : "do not terminate") << eom;
    debug() << "function " << 
      (has_loops ? "has loops" : "does not have loops") << eom;
  }

  // create summary
  summaryt summary;
  summary.params = SSA.params;
  summary.globals_in = SSA.globals_in;
  summary.globals_out = SSA.globals_out;

  if(!options.get_bool_option("havoc"))
  {
    do_summary(function_name,SSA,summary,forward,sufficient,postconditions);
  }

  //check termination
  if(options.get_bool_option("termination"))
  {
    if(has_loops ||
       options.get_bool_option("preconditions") &&
       has_function_calls) 
    {
      if(!options.get_bool_option("preconditions"))
      {
	if(calls_terminate)
          do_termination(function_name,SSA,summary);
	else
        {
	  summary.precondition = false_exprt();
	  summary.terminates = false;
	}
      }
      else if(has_loops)
      {
        do_termination_with_preconditions(function_name,SSA,
					  summary,postconditions);
      }
      else
      {
        do_termination_preconditions_only(function_name,SSA,
					  summary,postconditions);
      }
    }
    else if(!has_loops) summary.terminates = true;
  }

  {
    std::ostringstream out;
    out << std::endl << "Summary for function " << function_name << std::endl;
    summary.output(out,SSA.ns);   
    status() << out.str() << eom;
  }

  // store summary in db
  if(summary_db.exists(function_name)) 
  {
    summaryt old_summary = summary_db.get(function_name);
    join_summaries(old_summary,summary);
  }
  summary_db.put(function_name,summary);
}

/*******************************************************************\

Function: summarizert::do_summary()

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

void summarizert::do_summary(const function_namet &function_name, 
			     local_SSAt &SSA, summaryt &summary,
                             bool forward, bool sufficient, 
			     exprt::operandst postconditions)
{
  status() << "Computing summary" << eom;

  //analyze
  ssa_analyzert analyzer;
  analyzer.set_message_handler(get_message_handler());

  template_generator_summaryt template_generator(options,ssa_unwinder.get(function_name));
  template_generator.set_message_handler(get_message_handler());
  template_generator(SSA,forward);

  exprt cond = collect_postconditions(function_name,SSA,summary,
				      forward,sufficient,false,
				      postconditions);

  analyzer(SSA,cond,template_generator);
  if(forward) summary.precondition = preconditions.at(function_name);
  else 
  {
    analyzer.get_result(summary.precondition,template_generator.out_vars());
    if(sufficient) 
    {
      //TODO: also need under-approx transformer for sufficient precond
      summary.precondition = not_exprt(summary.precondition);
    }
  }
  analyzer.get_result(summary.transformer,template_generator.inout_vars());
#ifdef PRECISE_JOIN
  summary.transformer = implies_exprt(summary.precondition,summary.transformer);
#endif
  simplify_expr(summary.transformer, SSA.ns);

#if 0 
  simplify_expr(summary.precondition, SSA.ns); //does not help
#endif 

  analyzer.get_result(summary.invariant,template_generator.loop_vars());

  //statistics
  solver_instances++;
  solver_calls += analyzer.get_number_of_solver_calls();
}

/*******************************************************************\

Function: summarizert::do_termination()

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

void summarizert::do_termination(const function_namet &function_name, 
				 local_SSAt &SSA, summaryt &summary)
{
  status() << "Computing termination argument" << eom;

  template_generator_rankingt template_generator1(options,ssa_unwinder.get(function_name));
  template_generator1.set_message_handler(get_message_handler());
  template_generator1(SSA,true);

  if(template_generator1.all_vars().empty()) return; //nothing to do 

  //temporarily add invariant to SSA
  SSA.nodes.push_back(local_SSAt::nodet(SSA.nodes.back().location, SSA.nodes.end()));
  SSA.nodes.back().constraints.push_back(summary.invariant);

  // compute ranking functions
  ssa_analyzert analyzer1;
  analyzer1.set_message_handler(get_message_handler());
  analyzer1(SSA,preconditions[function_name],template_generator1);
  analyzer1.get_result(summary.termination_argument,template_generator1.all_vars());     

  //extract information whether a ranking function was found for all loops
  summary.terminates = check_termination_argument(summary.termination_argument); 

  //statistics
  solver_instances++;
  solver_calls += analyzer1.get_number_of_solver_calls();

  SSA.nodes.pop_back(); //remove invariant from SSA
}

/*******************************************************************\

Function: summarizert::do_termination_preconditions_only()

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

void summarizert::do_termination_preconditions_only(
  const function_namet &function_name, 
  local_SSAt &SSA, summaryt &summary, exprt::operandst postconditions)
{
  status() << "Computing precondition for termination" << eom;

  template_generator_summaryt template_generator2(options,ssa_unwinder.get(function_name));
  template_generator2.set_message_handler(get_message_handler());
  template_generator2(SSA,false); //backward

  //  SSA.nodes.front().equalities.erase(++SSA.nodes.front().equalities.begin()); //TEST

  exprt cond = collect_postconditions(function_name,SSA,summary,
				      false,true,true,
				      postconditions);
  status() << "POSTCOND: " << from_expr(SSA.ns,"",cond) << eom;
 
  //if template empty, check at least reachability (should actually be done by analyzer)
  if(template_generator2.all_vars().empty())
  {
    if(check_end_reachable(SSA,summary,cond,false))
    {
      summary.terminates = true;
      summary.precondition = true_exprt();
    }
    else
    {
      summary.terminates = false;
      summary.precondition = false_exprt();
    }
    return;
  }

  //temporarily add invariant to SSA
  SSA.nodes.push_back(local_SSAt::nodet(SSA.nodes.back().location, SSA.nodes.end()));
  SSA.nodes.back().constraints.push_back(summary.invariant);

  //compute sufficient preconditions w.r.t. termination argument
  ssa_analyzert analyzer2;
  analyzer2.set_message_handler(get_message_handler());
  analyzer2(SSA,cond,template_generator2);
  analyzer2.get_result(summary.precondition,template_generator2.out_vars());
  summary.precondition = not_exprt(summary.precondition);

  status() << "PRECOND: " << from_expr(SSA.ns,"",summary.precondition) << eom;

  //classify precondition == false as possibly non-terminating
  satcheckt satcheck2;
  bv_pointerst solver2(SSA.ns, satcheck2);
  solver2 << summary.precondition;
  summary.terminates = (solver2() == decision_proceduret::D_SATISFIABLE);
  if(!summary.terminates) summary.precondition = false_exprt();

  //statistics
  solver_instances += 2;
  solver_calls += analyzer2.get_number_of_solver_calls() + 1;

  SSA.nodes.pop_back(); //remove invariant from SSA
}

/*******************************************************************\

Function: summarizert::do_termination_with_preconditions()

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

void summarizert::do_termination_with_preconditions(
  const function_namet &function_name, 
  local_SSAt &SSA, summaryt &summary, exprt::operandst postconditions)
{
  exprt precondition = preconditions[function_name];

  template_generator_summaryt template_generator2(options,
    ssa_unwinder.get(function_name));
  template_generator2.set_message_handler(get_message_handler());
  template_generator2(SSA,false); //backward

  //temporarily add invariant to SSA
  SSA.nodes.push_back(local_SSAt::nodet(SSA.nodes.back().location, 
					SSA.nodes.end()));
  SSA.nodes.back().constraints.push_back(summary.invariant);

  template_generator_rankingt template_generator1(options,
    ssa_unwinder.get(function_name));
  template_generator1.set_message_handler(get_message_handler());
  template_generator1(SSA,true);

  if(!template_generator1.all_vars().empty()) //nothing to do 
  {
    //TODO (later): loop to find weaker preconditions
    satcheck_minisat_no_simplifiert satcheck1;
    bv_pointerst solver1(SSA.ns, satcheck1);
 
    //statistics
    solver_instances++;
  
    //bootstrap with a concrete model for input variables
    satcheck1.set_message_handler(get_message_handler());
    solver1.set_message_handler(get_message_handler());
    
    solver1 << SSA;
    solver1 << summary.invariant;
    solver1 << precondition;
    solver1 << SSA.guard_symbol(--SSA.goto_function.body.instructions.end()); //last node should be reachable
    const domaint::var_sett &invars = template_generator2.out_vars();

    debug() << from_expr(SSA.ns,"",
			 SSA.guard_symbol(--SSA.goto_function.body.instructions.end())) << eom;
    debug() << from_expr(SSA.ns,"",precondition) << eom;

    while(!summary.terminates)
    {
      //statistics
      solver_calls++;

      //solve
      switch(solver1())
      {
      case decision_proceduret::D_SATISFIABLE: {
	exprt::operandst c;
	for(domaint::var_sett::const_iterator it = invars.begin();
	    it != invars.end(); it++)
	{
	  c.push_back(equal_exprt(*it,solver1.get(*it)));
	}
	precondition = conjunction(c);
	debug() << "bootstrap model for precondition: " 
		<< from_expr(SSA.ns,"",precondition) << eom;
	break; }
      case decision_proceduret::D_UNSATISFIABLE: {
	debug() << "Function does not terminate" << eom;
	summary.termination_argument = true_exprt();
	summary.terminates = false;
	summary.precondition = false_exprt();
	return; 
      }
      default: assert(false); break;
      }

      status() << "Computing termination argument" << eom;

      // compute ranking functions
      ssa_analyzert analyzer1;
      analyzer1.set_message_handler(get_message_handler());
      analyzer1(SSA,precondition,template_generator1);
      analyzer1.get_result(summary.termination_argument,
			   template_generator1.all_vars());     

      //extract information whether a ranking function was found for all loops
      summary.terminates = 
        check_termination_argument(summary.termination_argument); 

      //statistics
      solver_instances++;
      solver_calls += analyzer1.get_number_of_solver_calls();

      if(!summary.terminates) 
      {
	solver1 << not_exprt(precondition);
	//this should not be in the precondition
#if 0
        if(summary.precondition.is_nil())
	  summary.precondition = not_exprt(precondition);
	else 
	  summary.precondition = and_exprt(summary.precondition,
					   not_exprt(precondition));
#endif
      }
    }
  }

  status() << "Computing precondition for termination" << eom;
  exprt cond = collect_postconditions(function_name,SSA,summary,
				      false,true,true,
				      postconditions);
  status() << "POSTCOND: " << from_expr(SSA.ns,"",cond) << eom;
  if(template_generator2.all_vars().empty())
  {
  //if template empty, check at least reachability (should actually be done by analyzer)
    if(!check_end_reachable(SSA,summary,cond,false))
    {
      summary.terminates = false;
      summary.precondition = false_exprt();
    }
    else
    {
      if(summary.terminates) summary.precondition = true_exprt();
      else summary.precondition = false_exprt();
    }
    return;
  }
  //compute sufficient preconditions w.r.t. termination argument
  ssa_analyzert analyzer2;
  analyzer2.set_message_handler(get_message_handler());
  analyzer2(SSA,cond,template_generator2);
  exprt new_precondition;
  analyzer2.get_result(new_precondition,template_generator2.out_vars());
  if(summary.precondition.is_nil())
    summary.precondition = not_exprt(new_precondition);
  else
    summary.precondition = and_exprt(summary.precondition,
				     not_exprt(new_precondition));
  debug() << "PRECOND: " << from_expr(SSA.ns,"",summary.precondition)<< eom;

  //classify precondition == false as possibly non-terminating
  satcheckt satcheck2;
  bv_pointerst solver2(SSA.ns, satcheck2);
  solver2 << summary.precondition;
  summary.terminates = (solver2() == decision_proceduret::D_SATISFIABLE);
  if(!summary.terminates) summary.precondition = true_exprt();

  //statistics
  solver_instances++;
  solver_calls += analyzer2.get_number_of_solver_calls();

  SSA.nodes.pop_back(); //remove invariant from SSA
}


/*******************************************************************\

Function: summarizert::inline_summaries()

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

void summarizert::inline_summaries(const function_namet &function_name, 
				   local_SSAt &SSA, bool context_sensitive,
				   bool forward, bool sufficient,
                                   bool &calls_terminate, 
				   bool &has_function_calls,
				   exprt::operandst &postconditions)
{
  ssa_inlinert inliner;
  inliner.set_message_handler(get_message_handler());

  local_SSAt::nodest::iterator n_it = SSA.nodes.begin();

  while(true) //iterate until all function calls are gone
  {
    local_SSAt::nodet::function_callst::iterator f_it; 
    bool found = false;
    while(n_it!=SSA.nodes.end())
    {
      if(n_it->function_calls.empty()) { n_it++; continue; }

      for(f_it = n_it->function_calls.begin();
        f_it != n_it->function_calls.end(); f_it++)
      {
        assert(f_it->function().id()==ID_symbol); //no function pointers
        found = true;
        break;
      }
      if(found) { has_function_calls = true; break; }
    }

    if(!found) break; //no more function calls

    irep_idt fname = to_symbol_expr(f_it->function()).get_identifier();

    if(!check_call_reachable(function_name,n_it,f_it,SSA)) 
    {
      n_it++;
      continue;
    }

    if(!check_precondition(function_name,n_it,f_it,SSA,
			   inliner,forward,sufficient,postconditions))
    {
      if(context_sensitive) 
        compute_calling_context(function_name,n_it,f_it,SSA,inliner,forward);

      status() << "Recursively summarizing function " << fname << eom;

      compute_summary_rec(fname,context_sensitive,forward,sufficient);
      summaryt fsummary = summary_db.get(fname);

      status() << "Replacing function " << fname << eom;
      //getting globals at call site
      local_SSAt::var_sett cs_globals_in, cs_globals_out; 
      goto_programt::const_targett loc = n_it->location;
      SSA.get_globals(loc,cs_globals_in);
      assert(loc!=SSA.goto_function.body.instructions.end());
      SSA.get_globals(++loc,cs_globals_out);

      /*
      //collect postconditions for precondition inference
      if(!forward)
      {
	//guard at call site
	exprt guard = SSA.guard_symbol(n_it->location);
	exprt precond = fsummary.precondition;

	inliner.rename_to_caller(f_it,fsummary.params,
				 cs_globals_in,fsummary.globals_in,precond);
	postconditions.push_back(implies_exprt(guard,precond));
      }
      */

      //replace
      inliner.replace(SSA,n_it,f_it,cs_globals_in,cs_globals_out,
		      fsummary,sufficient);
      summaries_used++;
      inliner.commit_node(n_it);
      assert(inliner.commit_nodes(SSA.nodes,n_it));
    }

    //get information about callee termination
    if(options.get_bool_option("termination"))
    {
      if(!summary_db.get(fname).terminates) 
      {
	calls_terminate = false;
	break;
      }
    }
    else calls_terminate = false;

    n_it++;
  }
}

/*******************************************************************\

Function: summarizert::join_summaries()

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

void summarizert::join_summaries(const summaryt &existing_summary, 
				 summaryt &new_summary)
{
  assert(existing_summary.params == new_summary.params);
  assert(existing_summary.globals_in == new_summary.globals_in);
  assert(existing_summary.globals_out == new_summary.globals_out);
  new_summary.precondition = or_exprt(existing_summary.precondition,
					   new_summary.precondition);
#ifdef PRECISE_JOIN
  new_summary.transformer = and_exprt(existing_summary.transformer,
    implies_exprt(new_summary.precondition,new_summary.transformer));
  new_summary.invariant = and_exprt(existing_summary.invariant,
    implies_exprt(new_summary.precondition,new_summary.invariant));
#else
  new_summary.transformer = or_exprt(existing_summary.transformer,
    new_summary.transformer);
  new_summary.invariant = or_exprt(existing_summary.invariant,
    new_summary.invariant);
#endif
}

/*******************************************************************\

Function: summarizert::check_precondition()

  Inputs:

 Outputs:

 Purpose: returns false if the summary needs to be recomputed

\******************************************************************/

bool summarizert::check_precondition(
  const function_namet &function_name,
  local_SSAt::nodest::iterator n_it, 
  local_SSAt::nodet::function_callst::iterator f_it,
  local_SSAt &SSA,
  ssa_inlinert &inliner,
  bool forward,
  bool sufficient,
  exprt::operandst &postconditions)
{
  assert(f_it->function().id()==ID_symbol); //no function pointers
  irep_idt fname = to_symbol_expr(f_it->function()).get_identifier();

  status() << "Checking precondition of " << fname << eom;

  bool precondition_holds = false;

  if(summary_db.exists(fname)) 
  {
    summaryt summary = summary_db.get(fname);
    if(summary.precondition.is_true()) //precondition trivially holds
    {
      //getting globals at call site
      local_SSAt::var_sett cs_globals_in, cs_globals_out; 
      goto_programt::const_targett loc = n_it->location;
      SSA.get_globals(loc,cs_globals_in);
      assert(loc!=SSA.goto_function.body.instructions.end());
      SSA.get_globals(++loc,cs_globals_out);

      status() << "Precondition trivially holds, replacing by summary." 
                   << eom;
      inliner.replace(SSA,n_it,f_it,
		      cs_globals_in,cs_globals_out,summary_db.get(fname),
		      sufficient);
      summaries_used++;
      precondition_holds = true;
    }
    else
    {
      exprt assertion = not_exprt(summary.precondition);

      //getting globals at call site
      local_SSAt::var_sett cs_globals_in; 
      SSA.get_globals(n_it->location,cs_globals_in);

      inliner.rename_to_caller(f_it,summary.params,
			       cs_globals_in,summary.globals_in,assertion);

      status() << "Precondition assertion for function " << fname << eom;
      n_it->assertions.push_back(assertion);

      precondition_holds = false;
    }
  }
  else if(!ssa_db.exists(fname))
  {
    status() << "Function " << fname << " not found" << eom;
    inliner.havoc(*n_it,f_it);
    precondition_holds = true;
  }
  else if(fname == function_name) 
  {
    status() << "Havoc recursive function call to " << fname << eom;
    inliner.havoc(*n_it,f_it);
    precondition_holds = true;
  }
  else 
  {
    status() << "Function " << fname << " not analyzed yet" << eom;
    return false; //function not seen yet
  }
  inliner.commit_node(n_it);
  assert(inliner.commit_nodes(SSA.nodes,n_it));

  if(precondition_holds) return true;

  // precondition check
  satcheckt satcheck;
  //smt2_dect solver(SSA.ns, "summarizer", "", "QF_BV", smt2_dect::Z3);
  bv_pointerst solver(SSA.ns, satcheck);
  //bv_pointerst solver(SSA.ns, smt);
  
  satcheck.set_message_handler(get_message_handler());
  solver.set_message_handler(get_message_handler());
    
  solver << SSA;
  solver << n_it->assertions.front();

  switch(solver())
  {
  case decision_proceduret::D_SATISFIABLE: {
    precondition_holds = false;

    n_it->assertions.clear();
    status() << "Precondition does not hold, need to recompute summary." << eom;
    break; }
  case decision_proceduret::D_UNSATISFIABLE: {
    precondition_holds = true;

    //getting globals at call site
    local_SSAt::var_sett cs_globals_in, cs_globals_out; 
    goto_programt::const_targett loc = n_it->location;
    SSA.get_globals(loc,cs_globals_in);
    assert(loc!=SSA.goto_function.body.instructions.end());
    SSA.get_globals(++loc,cs_globals_out);

    status() << "Precondition holds, replacing by summary." << eom;
    summaryt summary = summary_db.get(fname);

    /*
    //collect postconditions for precondition inference
    if(!forward)
    {
      //guard at call site
      exprt guard = SSA.guard_symbol(n_it->location);
      exprt precond = summary.precondition;

      inliner.rename_to_caller(f_it,summary.params,
			       cs_globals_in,summary.globals_in,precond);
      postconditions.push_back(implies_exprt(guard,precond));
      }*/


    inliner.replace(SSA,n_it,f_it,
		    cs_globals_in,cs_globals_out,summary,
		    sufficient);
    inliner.commit_node(n_it);
    assert(inliner.commit_nodes(SSA.nodes,n_it));

    summaries_used++;
                
    break; }
  default: assert(false); break;
  }

  //statistics
  solver_instances++;
  solver_calls++;

  return precondition_holds;
}

/*******************************************************************\

Function: summarizert::check_call_reachable()

  Inputs:

 Outputs:

 Purpose: returns false if function call is not reachable

\******************************************************************/

bool summarizert::check_call_reachable(
  const function_namet &function_name,
  local_SSAt::nodest::iterator n_it, 
  local_SSAt::nodet::function_callst::iterator f_it,
  local_SSAt &SSA)
{
  assert(f_it->function().id()==ID_symbol); //no function pointers
  irep_idt fname = to_symbol_expr(f_it->function()).get_identifier();

  debug() << "Checking reachability of call to " << fname << eom;

  bool reachable = false;
  // reachability check
  satcheckt satcheck;
  bv_pointerst solver(SSA.ns, satcheck);
  //smt2_dect solver(SSA.ns, "summarizer", "", "QF_BV", smt2_dect::Z3);
  //bv_pointerst solver(SSA.ns, smt);
  
  satcheck.set_message_handler(get_message_handler());
  solver.set_message_handler(get_message_handler());
    
  solver << SSA;
  symbol_exprt guard = SSA.guard_symbol(n_it->location);
  ssa_unwinder.get(function_name).unwinder_rename(guard,*n_it,false);
  solver << guard;

  switch(solver())
  {
  case decision_proceduret::D_SATISFIABLE: {
    reachable = true;
    debug() << "Call is reachable" << eom;
    break; }
  case decision_proceduret::D_UNSATISFIABLE: {
    debug() << "Call is not reachable" << eom;
    break; }
  default: assert(false); break;
  }

  //statistics
  solver_instances++;
  solver_calls++;

  return reachable;
}

/*******************************************************************\

Function: summarizert::compute_precondition ()

  Inputs:

 Outputs:

 Purpose: computes callee preconditions from the calling context
          for a single function call

\******************************************************************/

void summarizert::compute_calling_context(
  const function_namet &function_name, 
  local_SSAt::nodest::iterator n_it, 
  local_SSAt::nodet::function_callst::iterator f_it,
  local_SSAt &SSA,
  ssa_inlinert &inliner,
  bool forward)
{
  assert(f_it->function().id()==ID_symbol); //no function pointers
  irep_idt fname = to_symbol_expr(f_it->function()).get_identifier();

  status() << "Computing calling context for function " << fname << eom;

  ssa_analyzert analyzer;
  analyzer.set_message_handler(get_message_handler());

  template_generator_callingcontextt template_generator(options,ssa_unwinder.get(function_name));
  template_generator.set_message_handler(get_message_handler());
  template_generator(SSA,n_it,f_it,forward);

  // collect globals at call site
  std::map<local_SSAt::nodet::function_callst::iterator, local_SSAt::var_sett>
    cs_globals_in;
  if(forward) SSA.get_globals(n_it->location,cs_globals_in[f_it]);
  else SSA.get_globals((++n_it)->location,cs_globals_in[f_it]);

  // analyze
  analyzer(SSA,preconditions[function_name],template_generator);

  // set preconditions
  local_SSAt &fSSA = ssa_db.get(fname); 

  preconditiont precondition;
  analyzer.get_result(precondition,template_generator.callingcontext_vars());
  inliner.rename_to_callee(f_it, fSSA.params,
			     cs_globals_in[f_it],fSSA.globals_in,
			     precondition);

  debug() << "Calling context for " << 
    from_expr(SSA.ns, "", *f_it) << ": " 
	  << from_expr(SSA.ns, "", precondition) << eom;

  if(preconditions[fname].is_true())
    preconditions[fname] = precondition;
  else
    preconditions[fname] = or_exprt(preconditions[fname],precondition);

  //statistics
  solver_instances++;
  solver_calls += analyzer.get_number_of_solver_calls();
}

/*******************************************************************\

Function: summarizert::check_termination_argument()

  Inputs:

 Outputs:

 Purpose: checks whether a termination argument implies termination

\******************************************************************/

bool summarizert::check_termination_argument(exprt expr)
{
  if(expr.is_false()) return true;
  // should be of the form /\_i g_i => R_i
  if(expr.id()==ID_and)
  {
    for(exprt::operandst::iterator it = expr.operands().begin(); 
      it != expr.operands().end(); it++)
    {
      if(it->is_false()) return true;
      assert(it->id()==ID_implies);
      if(it->op1().is_true()) return false;
    }
  }
  else
  {
    if(expr.id()==ID_implies)
    {
      if(expr.op1().is_true()) return false;
    }
    else return !expr.is_true();
  }
  return true;
}

/*******************************************************************\

Function: summarizert::collect_postconditions()

  Inputs:

 Outputs:

 Purpose: collects postconditions where precondition inference starts from

\*******************************************************************/

exprt summarizert::collect_postconditions(const function_namet &function_name,
					  const local_SSAt &SSA, 
				      const summaryt &summary,
   				      bool forward,
   				      bool sufficient,
				      bool termination,
  				      exprt::operandst postconditions)
{
  if(forward) return preconditions[function_name];

  for(local_SSAt::nodest::const_iterator n_it = SSA.nodes.begin();
      n_it != SSA.nodes.end(); n_it++)
  {
    for(local_SSAt::nodet::assertionst::const_iterator 
	  a_it = n_it->assertions.begin();
	a_it != n_it->assertions.end(); a_it++)
    {
      postconditions.push_back(*a_it);
    }
  }
  if(termination) 
  {
    if(!summary.termination_argument.is_nil()) 
      postconditions.push_back(summary.termination_argument);
  }
  postconditions.push_back(SSA.guard_symbol(--SSA.goto_function.body.instructions.end()));
  exprt c;
  if(sufficient) c = not_exprt(conjunction(postconditions));
  else c = conjunction(postconditions);
  return c;
}

/*******************************************************************\

Function: summarizert::check_call_reachable()

  Inputs:

 Outputs:

 Purpose: returns false if the end of the function is not reachable

\******************************************************************/

bool summarizert::check_end_reachable(
   const local_SSAt &SSA, 
   const summaryt &summary,
   const exprt &precondition,
   bool forward)
{
  satcheckt satcheck;
  bv_pointerst solver(SSA.ns, satcheck);
  satcheck.set_message_handler(get_message_handler());
  solver.set_message_handler(get_message_handler());
  solver << SSA;
  solver << precondition;
  solver << summary.invariant;
  if(forward) 
    solver << 
      not_exprt(SSA.guard_symbol(--SSA.goto_function.body.instructions.end()));
  else
    solver << 
      not_exprt(SSA.guard_symbol(SSA.goto_function.body.instructions.begin()));

  //statistics
  solver_instances++;
  solver_calls++;

  return solver()==decision_proceduret::D_SATISFIABLE;
}
