/*
 * InputPath.cpp
 *
 *  Created on: 23 Oct 2015
 *      Author: hieu
 */
#include <boost/foreach.hpp>
#include "InputPath.h"
#include "TranslationModel/PhraseTable.h"

namespace Moses2
{

InputPath::InputPath(MemPool &pool,
		const SubPhrase &subPhrase,
		const Range &range,
		size_t numPt,
		const InputPath *prefixPath)
:subPhrase(subPhrase)
,range(range)
,prefixPath(prefixPath)
,m_isUsed(false)
{
  targetPhrases = pool.Allocate<const TargetPhrases*>(numPt);
  //Init<const TargetPhrases*>(targetPhrases, numPt, NULL);
  
  for (size_t i = 0; i < numPt; ++i) {
    targetPhrases[i] = NULL;
  }
  
}

InputPath::~InputPath() {
	// TODO Auto-generated destructor stub
}

void InputPath::AddTargetPhrases(const PhraseTable &pt, const TargetPhrases *tps)
{
	size_t ptInd = pt.GetPtInd();
	targetPhrases[ptInd] = tps;

	if (tps && tps->GetSize()) {
		m_isUsed = true;
	}
}

const TargetPhrases *InputPath::GetTargetPhrases(const PhraseTable &pt) const
{
	size_t ptInd = pt.GetPtInd();
	return targetPhrases[ptInd];
}

std::ostream& operator<<(std::ostream &out, const InputPath &obj)
{
	out << obj.range << " " << obj.subPhrase;
	return out;
}

}

