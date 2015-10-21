// $Id$
// vim:tabstop=2
/***********************************************************************
Moses - factored phrase-based language decoder
Copyright (C) 2006 University of Edinburgh

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
***********************************************************************/

#include <iostream>
#include <limits>
#include <vector>
#include <algorithm>

#include "TranslationOption.h"
#include "TranslationOptionCollection.h"
#include "Hypothesis.h"
#include "Util.h"
#include "SquareMatrix.h"
#include "StaticData.h"
#include "InputType.h"
#include "Manager.h"
#include "IOWrapper.h"
#include "moses/FF/FFState.h"
#include "moses/FF/StatefulFeatureFunction.h"
#include "moses/FF/StatelessFeatureFunction.h"

#include <boost/foreach.hpp>

using namespace std;

namespace Moses
{
Hypothesis *Hypothesis::Create(Manager& manager, InputType const& source, const TranslationOption &initialTransOpt, const WordsBitmap &bitmap)
{
	util::ObjectPool<Hypothesis> &pool = manager.GetHypoPool();
	Hypothesis *mem = pool.Get();
	Hypothesis *hypo = new(mem) Hypothesis(manager, source, initialTransOpt, bitmap);
	return hypo;
}

Hypothesis *Hypothesis::Create(const Hypothesis &prevHypo, const TranslationOption &transOpt, const WordsBitmap &bitmap)
{
	Hypothesis *hypo = new Hypothesis(prevHypo, transOpt, bitmap);
	return hypo;
}

void Hypothesis::Destroy(const Hypothesis *hypo)
{
	//delete hypo;
}

Hypothesis::
Hypothesis(Manager& manager, InputType const& source, const TranslationOption &initialTransOpt, const WordsBitmap &bitmap)
  : m_prevHypo(NULL)
  , m_sourceCompleted(bitmap)
  , m_sourceInput(source)
  , m_currSourceWordsRange(
    m_sourceCompleted.GetFirstGapPos()>0 ? 0 : NOT_FOUND,
    m_sourceCompleted.GetFirstGapPos()>0 ? m_sourceCompleted.GetFirstGapPos()-1 : NOT_FOUND)
  , m_currTargetWordsRange(NOT_FOUND, NOT_FOUND)
  , m_wordDeleted(false)
  , m_totalScore(0.0f)
  , m_futureScore(0.0f)
  , m_ffStates(StatefulFeatureFunction::GetStatefulFeatureFunctions().size())
  , m_arcList(NULL)
  , m_transOpt(initialTransOpt)
  , m_manager(manager)
  , m_id(m_manager.GetNextHypoId())
{
  // used for initial seeding of trans process
  // initialize scores
  //_hash_computed = false;
  //s_HypothesesCreated = 1;
  const vector<const StatefulFeatureFunction*>& ffs = StatefulFeatureFunction::GetStatefulFeatureFunctions();
  for (unsigned i = 0; i < ffs.size(); ++i)
    m_ffStates[i] = ffs[i]->EmptyHypothesisState(source);
  m_manager.GetSentenceStats().AddCreated();
}

/***
 * continue prevHypo by appending the phrases in transOpt
 */
Hypothesis::
Hypothesis(const Hypothesis &prevHypo, const TranslationOption &transOpt, const WordsBitmap &bitmap)
  : m_prevHypo(&prevHypo)
  , m_sourceCompleted(bitmap)
  , m_sourceInput(prevHypo.m_sourceInput)
  , m_currSourceWordsRange(transOpt.GetSourceWordsRange())
  , m_currTargetWordsRange(prevHypo.m_currTargetWordsRange.GetEndPos() + 1,
                           prevHypo.m_currTargetWordsRange.GetEndPos()
                           + transOpt.GetTargetPhrase().GetSize())
  , m_wordDeleted(false)
  , m_totalScore(0.0f)
  , m_futureScore(0.0f)
  , m_ffStates(prevHypo.m_ffStates.size())
  , m_arcList(NULL)
  , m_transOpt(transOpt)
  , m_manager(prevHypo.GetManager())
  , m_id(m_manager.GetNextHypoId())
{
  m_currScoreBreakdown.PlusEquals(transOpt.GetScoreBreakdown());

  // assert that we are not extending our hypothesis by retranslating something
  // that this hypothesis has already translated!
  assert(!m_sourceCompleted.Overlap(m_currSourceWordsRange));

  m_wordDeleted = transOpt.IsDeletionOption();
  m_manager.GetSentenceStats().AddCreated();
}

Hypothesis::
~Hypothesis()
{
  for (unsigned i = 0; i < m_ffStates.size(); ++i)
    delete m_ffStates[i];

  if (m_arcList) {
    ArcList::iterator iter;
    for (iter = m_arcList->begin() ; iter != m_arcList->end() ; ++iter) {
    	Hypothesis::Destroy(*iter);
    }
    m_arcList->clear();

    delete m_arcList;
    m_arcList = NULL;
  }
}

void
Hypothesis::
AddArc(Hypothesis *loserHypo)
{
  if (!m_arcList) {
    if (loserHypo->m_arcList) { // we don't have an arcList, but loser does
      this->m_arcList = loserHypo->m_arcList;  // take ownership, we'll delete
      loserHypo->m_arcList = 0;                // prevent a double deletion
    } else {
      this->m_arcList = new ArcList();
    }
  } else {
    if (loserHypo->m_arcList) {  // both have an arc list: merge. delete loser
      size_t my_size = m_arcList->size();
      size_t add_size = loserHypo->m_arcList->size();
      this->m_arcList->resize(my_size + add_size, 0);
      std::memcpy(&(*m_arcList)[0] + my_size, &(*loserHypo->m_arcList)[0], add_size * sizeof(Hypothesis *));
      delete loserHypo->m_arcList;
      loserHypo->m_arcList = 0;
    } else { // loserHypo doesn't have any arcs
      // DO NOTHING
    }
  }
  m_arcList->push_back(loserHypo);
}

void
Hypothesis::
EvaluateWhenApplied(StatefulFeatureFunction const& sfff,
                    int state_idx)
{
  const StaticData &staticData = StaticData::Instance();
  if (! staticData.IsFeatureFunctionIgnored( sfff )) {
    Manager& manager = this->GetManager(); //Get the manager and the ttask
    ttasksptr const& ttask = manager.GetTtask();

    m_ffStates[state_idx] = sfff.EvaluateWhenAppliedWithContext
                            (ttask, *this, m_prevHypo ? m_prevHypo->m_ffStates[state_idx] : NULL,
                             &m_currScoreBreakdown);
  }
}

void
Hypothesis::
EvaluateWhenApplied(const StatelessFeatureFunction& slff)
{
  const StaticData &staticData = StaticData::Instance();
  if (! staticData.IsFeatureFunctionIgnored( slff )) {
    slff.EvaluateWhenApplied(*this, &m_currScoreBreakdown);
  }
}

/***
 * calculate the logarithm of our total translation score (sum up components)
 */
void
Hypothesis::
EvaluateWhenApplied(float futureScore)
{
  IFVERBOSE(2) {
    m_manager.GetSentenceStats().StartTimeOtherScore();
  }
  // some stateless score producers cache their values in the translation
  // option: add these here
  // language model scores for n-grams completely contained within a target
  // phrase are also included here

  // compute values of stateless feature functions that were not
  // cached in the translation option
  const vector<const StatelessFeatureFunction*>& sfs =
    StatelessFeatureFunction::GetStatelessFeatureFunctions();
  for (unsigned i = 0; i < sfs.size(); ++i) {
    const StatelessFeatureFunction &ff = *sfs[i];
    EvaluateWhenApplied(ff);
  }

  const vector<const StatefulFeatureFunction*>& ffs =
    StatefulFeatureFunction::GetStatefulFeatureFunctions();
  for (unsigned i = 0; i < ffs.size(); ++i) {
    const StatefulFeatureFunction &ff = *ffs[i];
    const StaticData &staticData = StaticData::Instance();
    if (! staticData.IsFeatureFunctionIgnored(ff)) {
      m_ffStates[i] = ff.EvaluateWhenApplied(*this,
                                             m_prevHypo ? m_prevHypo->m_ffStates[i] : NULL,
                                             &m_currScoreBreakdown);
    }
  }

  IFVERBOSE(2) {
    m_manager.GetSentenceStats().StopTimeOtherScore();
    m_manager.GetSentenceStats().StartTimeEstimateScore();
  }

  // FUTURE COST
  m_futureScore = futureScore;

  // TOTAL
  m_totalScore = m_currScoreBreakdown.GetWeightedScore() + m_futureScore;
  if (m_prevHypo) m_totalScore += m_prevHypo->GetScore();

  IFVERBOSE(2) {
    m_manager.GetSentenceStats().StopTimeEstimateScore();
  }
}

const Hypothesis* Hypothesis::GetPrevHypo()const
{
  return m_prevHypo;
}

/**
 * print hypothesis information for pharaoh-style logging
 */
void
Hypothesis::
PrintHypothesis() const
{
  if (!m_prevHypo) {
    TRACE_ERR(endl << "NULL hypo" << endl);
    return;
  }
  TRACE_ERR(endl << "creating hypothesis "<< m_id <<" from "<< m_prevHypo->m_id<<" ( ");
  int end = (int)(m_prevHypo->GetCurrTargetPhrase().GetSize()-1);
  int start = end-1;
  if ( start < 0 ) start = 0;
  if ( m_prevHypo->m_currTargetWordsRange.GetStartPos() == NOT_FOUND ) {
    TRACE_ERR( "<s> ");
  } else {
    TRACE_ERR( "... ");
  }
  if (end>=0) {
    WordsRange range(start, end);
    TRACE_ERR( m_prevHypo->GetCurrTargetPhrase().GetSubString(range) << " ");
  }
  TRACE_ERR( ")"<<endl);
  TRACE_ERR( "\tbase score "<< (m_prevHypo->m_totalScore - m_prevHypo->m_futureScore) <<endl);
  TRACE_ERR( "\tcovering "<<m_currSourceWordsRange.GetStartPos()<<"-"<<m_currSourceWordsRange.GetEndPos()
             <<": " << m_transOpt.GetInputPath().GetPhrase() << endl);

  TRACE_ERR( "\ttranslated as: "<<(Phrase&) GetCurrTargetPhrase()<<endl); // <<" => translation cost "<<m_score[ScoreType::PhraseTrans];

  if (m_wordDeleted) TRACE_ERR( "\tword deleted"<<endl);
  //	TRACE_ERR( "\tdistance: "<<GetCurrSourceWordsRange().CalcDistortion(m_prevHypo->GetCurrSourceWordsRange())); // << " => distortion cost "<<(m_score[ScoreType::Distortion]*weightDistortion)<<endl;
  //	TRACE_ERR( "\tlanguage model cost "); // <<m_score[ScoreType::LanguageModelScore]<<endl;
  //	TRACE_ERR( "\tword penalty "); // <<(m_score[ScoreType::WordPenalty]*weightWordPenalty)<<endl;
  TRACE_ERR( "\tscore "<<m_totalScore - m_futureScore<<" + future cost "<<m_futureScore<<" = "<<m_totalScore<<endl);
  TRACE_ERR(  "\tunweighted feature scores: " << m_currScoreBreakdown << endl);
  //PrintLMScores();
}

void
Hypothesis::
CleanupArcList()
{
  // point this hypo's main hypo to itself
  SetWinningHypo(this);

  if (!m_arcList) return;

  /* keep only number of arcs we need to create all n-best paths.
   * However, may not be enough if only unique candidates are needed,
   * so we'll keep all of arc list if nedd distinct n-best list
   */
  const StaticData &staticData = StaticData::Instance();
  size_t nBestSize = staticData.options().nbest.nbest_size;
  bool distinctNBest = (m_manager.options().nbest.only_distinct ||
                        staticData.GetLatticeSamplesSize() ||
                        m_manager.options().mbr.enabled ||
                        staticData.GetOutputSearchGraph() ||
                        staticData.GetOutputSearchGraphSLF() ||
                        staticData.GetOutputSearchGraphHypergraph() ||
                        m_manager.options().lmbr.enabled);

  if (!distinctNBest && m_arcList->size() > nBestSize * 5) {
    // prune arc list only if there too many arcs
    NTH_ELEMENT4(m_arcList->begin(), m_arcList->begin() + nBestSize - 1,
                 m_arcList->end(), CompareHypothesisTotalScore());

    // delete bad ones
    ArcList::iterator iter;
    for (iter = m_arcList->begin() + nBestSize; iter != m_arcList->end() ; ++iter)
    	Hypothesis::Destroy(*iter);
    m_arcList->erase(m_arcList->begin() + nBestSize, m_arcList->end());
  }

  // set all arc's main hypo variable to this hypo
  ArcList::iterator iter = m_arcList->begin();
  for (; iter != m_arcList->end() ; ++iter) {
    Hypothesis *arc = *iter;
    arc->SetWinningHypo(this);
  }
}

TargetPhrase const&
Hypothesis::
GetCurrTargetPhrase() const
{
  return m_transOpt.GetTargetPhrase();
}

void
Hypothesis::
GetOutputPhrase(Phrase &out) const
{
  if (m_prevHypo != NULL)
    m_prevHypo->GetOutputPhrase(out);
  out.Append(GetCurrTargetPhrase());
}

TO_STRING_BODY(Hypothesis)

// friend
ostream& operator<<(ostream& out, const Hypothesis& hypo)
{
  hypo.ToStream(out);
  // words bitmap
  out << "[" << hypo.m_sourceCompleted << "] ";

  // scores
  out << " [total=" << hypo.GetTotalScore() << "]";
  out << " " << hypo.GetScoreBreakdown();

  // alignment
  out << " " << hypo.GetCurrTargetPhrase().GetAlignNonTerm();

  return out;
}


std::string
Hypothesis::
GetSourcePhraseStringRep(const vector<FactorType> factorsToPrint) const
{
  return m_transOpt.GetInputPath().GetPhrase().GetStringRep(factorsToPrint);
}

std::string
Hypothesis::
GetTargetPhraseStringRep(const vector<FactorType> factorsToPrint) const
{
  return (m_prevHypo ? GetCurrTargetPhrase().GetStringRep(factorsToPrint) : "");
}

std::string
Hypothesis::
GetSourcePhraseStringRep() const
{
  vector<FactorType> allFactors(MAX_NUM_FACTORS);
  for(size_t i=0; i < MAX_NUM_FACTORS; i++)
    allFactors[i] = i;
  return GetSourcePhraseStringRep(allFactors);
}

std::string
Hypothesis::
GetTargetPhraseStringRep() const
{
  vector<FactorType> allFactors(MAX_NUM_FACTORS);
  for(size_t i=0; i < MAX_NUM_FACTORS; i++)
    allFactors[i] = i;
  return GetTargetPhraseStringRep(allFactors);
}

void
Hypothesis::
OutputAlignment(std::ostream &out) const
{
  std::vector<const Hypothesis *> edges;
  const Hypothesis *currentHypo = this;
  while (currentHypo) {
    edges.push_back(currentHypo);
    currentHypo = currentHypo->GetPrevHypo();
  }

  OutputAlignment(out, edges);

}

void
Hypothesis::
OutputAlignment(ostream &out, const vector<const Hypothesis *> &edges)
{
  size_t targetOffset = 0;

  for (int currEdge = (int)edges.size() - 1 ; currEdge >= 0 ; currEdge--) {
    const Hypothesis &edge = *edges[currEdge];
    const TargetPhrase &tp = edge.GetCurrTargetPhrase();
    size_t sourceOffset = edge.GetCurrSourceWordsRange().GetStartPos();

    OutputAlignment(out, tp.GetAlignTerm(), sourceOffset, targetOffset);

    targetOffset += tp.GetSize();
  }
  // Used by --print-alignment-info, so no endl
}

void
Hypothesis::
OutputAlignment(ostream &out, const AlignmentInfo &ai,
                size_t sourceOffset, size_t targetOffset)
{
  typedef std::vector< const std::pair<size_t,size_t>* > AlignVec;
  AlignVec alignments = ai.GetSortedAlignments();

  AlignVec::const_iterator it;
  for (it = alignments.begin(); it != alignments.end(); ++it) {
    const std::pair<size_t,size_t> &alignment = **it;
    out << alignment.first + sourceOffset << "-" << alignment.second + targetOffset << " ";
  }

}

void
Hypothesis::
OutputInput(std::vector<const Phrase*>& map, const Hypothesis* hypo)
{
  if (!hypo->GetPrevHypo()) return;
  OutputInput(map, hypo->GetPrevHypo());
  map[hypo->GetCurrSourceWordsRange().GetStartPos()]
  = &hypo->GetTranslationOption().GetInputPath().GetPhrase();
}

void
Hypothesis::
OutputInput(std::ostream& os) const
{
  size_t len = this->GetInput().GetSize();
  std::vector<const Phrase*> inp_phrases(len, 0);
  OutputInput(inp_phrases, this);
  for (size_t i=0; i<len; ++i)
    if (inp_phrases[i]) os << *inp_phrases[i];
}

void
Hypothesis::
OutputBestSurface(std::ostream &out, const std::vector<FactorType> &outputFactorOrder,
                  char reportSegmentation, bool reportAllFactors) const
{
  if (m_prevHypo) {
    // recursively retrace this best path through the lattice, starting from the end of the hypothesis sentence
    m_prevHypo->OutputBestSurface(out, outputFactorOrder, reportSegmentation, reportAllFactors);
  }
  OutputSurface(out, *this, outputFactorOrder, reportSegmentation, reportAllFactors);
}

//////////////////////////////////////////////////////////////////////////
/***
 * print surface factor only for the given phrase
 */
void
Hypothesis::
OutputSurface(std::ostream &out, const Hypothesis &edge,
              const std::vector<FactorType> &outputFactorOrder,
              char reportSegmentation, bool reportAllFactors) const
{
  UTIL_THROW_IF2(outputFactorOrder.size() == 0,
                 "Must specific at least 1 output factor");
  const TargetPhrase& phrase = edge.GetCurrTargetPhrase();
  bool markUnknown = StaticData::Instance().GetMarkUnknown();
  if (reportAllFactors == true) {
    out << phrase;
  } else {
    FactorType placeholderFactor = StaticData::Instance().GetPlaceholderFactor();

    std::map<size_t, const Factor*> placeholders;
    if (placeholderFactor != NOT_FOUND) {
      // creates map of target position -> factor for placeholders
      placeholders = GetPlaceholders(edge, placeholderFactor);
    }

    size_t size = phrase.GetSize();
    for (size_t pos = 0 ; pos < size ; pos++) {
      const Factor *factor = phrase.GetFactor(pos, outputFactorOrder[0]);

      if (placeholders.size()) {
        // do placeholders
        std::map<size_t, const Factor*>::const_iterator iter = placeholders.find(pos);
        if (iter != placeholders.end()) {
          factor = iter->second;
        }
      }

      UTIL_THROW_IF2(factor == NULL,
                     "No factor 0 at position " << pos);

      //preface surface form with UNK if marking unknowns
      const Word &word = phrase.GetWord(pos);
      if(markUnknown && word.IsOOV()) {
        out << StaticData::Instance().GetUnknownWordPrefix()
            << *factor
            << StaticData::Instance().GetUnknownWordSuffix();
      } else {
        out << *factor;
      }

      for (size_t i = 1 ; i < outputFactorOrder.size() ; i++) {
        const Factor *factor = phrase.GetFactor(pos, outputFactorOrder[i]);
        UTIL_THROW_IF2(factor == NULL,
                       "No factor " << i << " at position " << pos);

        out << "|" << *factor;
      }
      out << " ";
    }
  }

  // trace ("report segmentation") option "-t" / "-tt"
  if (reportSegmentation > 0 && phrase.GetSize() > 0) {
    const WordsRange &sourceRange = edge.GetCurrSourceWordsRange();
    const int sourceStart = sourceRange.GetStartPos();
    const int sourceEnd = sourceRange.GetEndPos();
    out << "|" << sourceStart << "-" << sourceEnd;    // enriched "-tt"
    if (reportSegmentation == 2) {
      out << ",wa=";
      const AlignmentInfo &ai = edge.GetCurrTargetPhrase().GetAlignTerm();
      Hypothesis::OutputAlignment(out, ai, 0, 0);
      out << ",total=";
      out << edge.GetScore() - edge.GetPrevHypo()->GetScore();
      out << ",";
      ScoreComponentCollection scoreBreakdown(edge.GetScoreBreakdown());
      scoreBreakdown.MinusEquals(edge.GetPrevHypo()->GetScoreBreakdown());
      scoreBreakdown.OutputAllFeatureScores(out);
    }
    out << "| ";
  }
}

std::map<size_t, const Factor*>
Hypothesis::
GetPlaceholders(const Hypothesis &hypo, FactorType placeholderFactor) const
{
  const InputPath &inputPath = hypo.GetTranslationOption().GetInputPath();
  const Phrase &inputPhrase = inputPath.GetPhrase();

  std::map<size_t, const Factor*> ret;

  for (size_t sourcePos = 0; sourcePos < inputPhrase.GetSize(); ++sourcePos) {
    const Factor *factor = inputPhrase.GetFactor(sourcePos, placeholderFactor);
    if (factor) {
      std::set<size_t> targetPos = hypo.GetTranslationOption().GetTargetPhrase().GetAlignTerm().GetAlignmentsForSource(sourcePos);
      UTIL_THROW_IF2(targetPos.size() != 1,
                     "Placeholder should be aligned to 1, and only 1, word");
      ret[*targetPos.begin()] = factor;
    }
  }

  return ret;
}

size_t Hypothesis::hash() const
{
  size_t seed;

  // coverage
  seed = m_sourceCompleted.hash();

  // states
  for (size_t i = 0; i < m_ffStates.size(); ++i) {
    const FFState *state = m_ffStates[i];
    size_t hash = state->hash();
    boost::hash_combine(seed, hash);
  }
  return seed;
}

bool Hypothesis::operator==(const Hypothesis& other) const
{
  // coverage
  if (m_sourceCompleted != other.m_sourceCompleted) {
    return false;
  }

  // states
  for (size_t i = 0; i < m_ffStates.size(); ++i) {
    const FFState &thisState = *m_ffStates[i];
    const FFState &otherState = *other.m_ffStates[i];
    if (thisState != otherState) {
      return false;
    }
  }
  return true;
}

#ifdef HAVE_XMLRPC_C
void
Hypothesis::
OutputLocalWordAlignment(vector<xmlrpc_c::value>& dest) const
{
  using namespace std;
  WordsRange const& src = this->GetCurrSourceWordsRange();
  WordsRange const& trg = this->GetCurrTargetWordsRange();

  vector<pair<size_t,size_t> const* > a
  = this->GetCurrTargetPhrase().GetAlignTerm().GetSortedAlignments();
  typedef pair<size_t,size_t> item;
  map<string, xmlrpc_c::value> M;
  BOOST_FOREACH(item const* p, a) {
    M["source-word"] = xmlrpc_c::value_int(src.GetStartPos() + p->first);
    M["target-word"] = xmlrpc_c::value_int(trg.GetStartPos() + p->second);
    dest.push_back(xmlrpc_c::value_struct(M));
  }
}

void
Hypothesis::
OutputWordAlignment(vector<xmlrpc_c::value>& out) const
{
  vector<Hypothesis const*> tmp;
  for (Hypothesis const* h = this; h; h = h->GetPrevHypo())
    tmp.push_back(h);
  for (size_t i = tmp.size(); i-- > 0;)
    tmp[i]->OutputLocalWordAlignment(out);
}

#endif


}

