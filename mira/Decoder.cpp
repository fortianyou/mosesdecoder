/***********************************************************************
Moses - factored phrase-based language decoder
Copyright (C) 2009 University of Edinburgh

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

#include "Decoder.h"
#include "Manager.h"
#include "ChartManager.h"
#include "Sentence.h"
#include "InputType.h"
#include "TranslationSystem.h"
#include "Phrase.h"
#include "TrellisPathList.h"
#include "ChartTrellisPathList.h"
#include "ChartTrellisPath.h"
#include "IOWrapper.h"

using namespace std;
using namespace Moses;


namespace Mira {

  /**
    * Allocates a char* and copies string into it.
  **/
  static char* strToChar(const string& s) {
    char* c = new char[s.size()+1];
    strcpy(c,s.c_str());
    return c;
  }

  MosesDecoder::MosesDecoder(const string& inifile, int debuglevel, int argc, vector<string> decoder_params)
		: m_manager(NULL) {
  	static int BASE_ARGC = 4;
	  Parameter* params = new Parameter();
	  char ** mosesargv = new char*[BASE_ARGC + argc];
	  mosesargv[0] = strToChar("-f");
	  mosesargv[1] = strToChar(inifile);
	  mosesargv[2] = strToChar("-v");
	  stringstream dbgin;
	  dbgin << debuglevel;
	  mosesargv[3] = strToChar(dbgin.str());

	  for (int i = 0; i < argc; ++i) {
		  char *cstr = &(decoder_params[i])[0];
		  mosesargv[BASE_ARGC + i] = cstr;
	  }

	  if (!params->LoadParam(BASE_ARGC + argc,mosesargv)) {
		  cerr << "Loading static data failed, exit." << endl;
		  exit(1);
	  }
	  StaticData::LoadDataStatic(params);
	  for (int i = 0; i < BASE_ARGC; ++i) {
		  delete[] mosesargv[i];
	  }
	  delete[] mosesargv;

	  const StaticData &staticData = StaticData::Instance();
      m_bleuScoreFeature = staticData.GetBleuScoreFeature();
  }
  
  void MosesDecoder::cleanup(bool chartDecoding) {
	  delete m_manager;
	  if (chartDecoding)
	  	delete m_chartManager;
	  else
	  	delete m_sentence;
  }

  vector< vector<const Word*> > MosesDecoder::getNBest(const std::string& source,
                              size_t sentenceid,
                              size_t nBestSize,
                              float bleuObjectiveWeight, 
                              float bleuScoreWeight,
                              vector< ScoreComponentCollection>& featureValues,
                              vector< float>& bleuScores,
                              vector< float>& modelScores,
                              size_t numReturnedTranslations,
                              bool distinct,
                              bool avgRefLength,
                              size_t rank,
                              size_t epoch)
  {
  	StaticData &staticData = StaticData::InstanceNonConst();
  	initialize(staticData, source, sentenceid, bleuObjectiveWeight, bleuScoreWeight, avgRefLength);
    const TranslationSystem& system = staticData.GetTranslationSystem(TranslationSystem::DEFAULT);

    // run the decoder
    if (staticData.GetSearchAlgorithm() == ChartDecoding) {
    	return runChartDecoder(source, sentenceid, nBestSize, bleuObjectiveWeight, bleuScoreWeight,
    			featureValues, bleuScores, modelScores, numReturnedTranslations, distinct, rank, epoch,
    			system);
    }
    else {
    	SearchAlgorithm search = staticData.GetSearchAlgorithm();
    	return runDecoder(source, sentenceid, nBestSize, bleuObjectiveWeight, bleuScoreWeight,
    			featureValues, bleuScores, modelScores, numReturnedTranslations, distinct, rank, epoch,
    			search, system);
    }
  }

  vector< vector<const Word*> > MosesDecoder::runDecoder(const std::string& source,
  														size_t sentenceid,
  														size_t nBestSize,
  														float bleuObjectiveWeight,
  														float bleuScoreWeight,
  														vector< ScoreComponentCollection>& featureValues,
  														vector< float>& bleuScores,
  														vector< float>& modelScores,
  														size_t numReturnedTranslations,
  														bool distinct,
  														size_t rank,
  														size_t epoch,
  														SearchAlgorithm& search,
  														const TranslationSystem& system) {
  	// run the decoder
    m_manager = new Moses::Manager(*m_sentence, search, &system);
    m_manager->ProcessSentence();
    TrellisPathList nBestList;
    m_manager->CalcNBest(nBestSize, nBestList, distinct);

    // read off the feature values and bleu scores for each sentence in the nbest list
    Moses::TrellisPathList::const_iterator iter;
    for (iter = nBestList.begin() ; iter != nBestList.end() ; ++iter) {
    	const Moses::TrellisPath &path = **iter;
    	featureValues.push_back(path.GetScoreBreakdown());
    	float bleuScore = getBleuScore(featureValues.back());
    	bleuScores.push_back(bleuScore);

    	//std::cout << "Score breakdown: " << path.GetScoreBreakdown() << endl;
    	float scoreWithoutBleu = path.GetTotalScore() - (bleuObjectiveWeight * bleuScoreWeight * bleuScore);
    	modelScores.push_back(scoreWithoutBleu);

    	Phrase bestPhrase = path.GetTargetPhrase();

    	if (iter != nBestList.begin())
    		cerr << endl;
    		cerr << "Rank " << rank << ", epoch " << epoch << ", \"";
    		Phrase phrase = path.GetTargetPhrase();
    		for (size_t pos = 0; pos < phrase.GetSize(); ++pos) {
    		const Word &word = phrase.GetWord(pos);
    		Word *newWord = new Word(word);
    		cerr << *newWord;
    	}

    	cerr << "\", score: " << scoreWithoutBleu << ", Bleu: " << bleuScore << ", total: " << path.GetTotalScore();

    	// set bleu score to zero in the feature vector since we do not want to optimise its weight
    	setBleuScore(featureValues.back(), 0);
    }

    // prepare translations to return
    vector< vector<const Word*> > translations;
    for (size_t i=0; i < numReturnedTranslations && i < nBestList.GetSize(); ++i) {
        const TrellisPath &path = nBestList.at(i);
        Phrase phrase = path.GetTargetPhrase();

        vector<const Word*> translation;
        for (size_t pos = 0; pos < phrase.GetSize(); ++pos) {
        	const Word &word = phrase.GetWord(pos);
        	Word *newWord = new Word(word);
        	translation.push_back(newWord);
        }
        translations.push_back(translation);
    }

//    cerr << "Rank " << rank << ", use cache: " << staticData.GetUseTransOptCache() << ", weights: " << staticData.GetAllWeights() << endl;
    return translations;
  }

  vector< vector<const Word*> > MosesDecoder::runChartDecoder(const std::string& source,
                              size_t sentenceid,
                              size_t nBestSize,
                              float bleuObjectiveWeight,
                              float bleuScoreWeight,
                              vector< ScoreComponentCollection>& featureValues,
                              vector< float>& bleuScores,
                              vector< float>& modelScores,
                              size_t numReturnedTranslations,
                              bool distinct,
                              size_t rank,
                              size_t epoch,
                              const TranslationSystem& system) {
  	// run the decoder
    m_chartManager = new ChartManager(*m_sentence, &system);
    m_chartManager->ProcessSentence();
    ChartTrellisPathList nBestList;
    m_chartManager->CalcNBest(nBestSize, nBestList, distinct);

    // read off the feature values and bleu scores for each sentence in the nbest list
    ChartTrellisPathList::const_iterator iter;
    for (iter = nBestList.begin() ; iter != nBestList.end() ; ++iter) {
    	const Moses::ChartTrellisPath &path = **iter;
    	featureValues.push_back(path.GetScoreBreakdown());
    	float bleuScore = getBleuScore(featureValues.back());
    	bleuScores.push_back(bleuScore);

    	//std::cout << "Score breakdown: " << path.GetScoreBreakdown() << endl;
    	float scoreWithoutBleu = path.GetTotalScore() - (bleuObjectiveWeight * bleuScoreWeight * bleuScore);
    	modelScores.push_back(scoreWithoutBleu);

    	Phrase bestPhrase = path.GetOutputPhrase();

    	if (iter != nBestList.begin())
    		cerr << endl;
    		cerr << "Rank " << rank << ", epoch " << epoch << ", \"";
    		Phrase phrase = path.GetOutputPhrase();
    		for (size_t pos = 0; pos < phrase.GetSize(); ++pos) {
    		const Word &word = phrase.GetWord(pos);
    		Word *newWord = new Word(word);
    		cerr << *newWord;
    	}

    	cerr << "\", score: " << scoreWithoutBleu << ", Bleu: " << bleuScore << ", total: " << path.GetTotalScore();

    	// set bleu score to zero in the feature vector since we do not want to optimise its weight
    	setBleuScore(featureValues.back(), 0);
    }

    // prepare translations to return
    vector< vector<const Word*> > translations;
    for (iter = nBestList.begin() ; iter != nBestList.end() ; ++iter) {
        const ChartTrellisPath &path = **iter;
        Phrase phrase = path.GetOutputPhrase();

        vector<const Word*> translation;
        for (size_t pos = 0; pos < phrase.GetSize(); ++pos) {
        	const Word &word = phrase.GetWord(pos);
        	Word *newWord = new Word(word);
        	translation.push_back(newWord);
        }
        translations.push_back(translation);
    }

//    cerr << "Rank " << rank << ", use cache: " << staticData.GetUseTransOptCache() << ", weights: " << staticData.GetAllWeights() << endl;
    return translations;
  }

  void MosesDecoder::outputNBestList(const std::string& source, size_t sentenceid,
  														size_t nBestSize, float bleuObjectiveWeight, float bleuScoreWeight,
  														bool distinctNbest, bool avgRefLength, string filename, ofstream& streamOut) {
  	StaticData &staticData = StaticData::InstanceNonConst();
  	initialize(staticData, source, sentenceid, bleuObjectiveWeight, bleuScoreWeight, avgRefLength);
    const TranslationSystem& system = staticData.GetTranslationSystem(TranslationSystem::DEFAULT);

    if (staticData.GetSearchAlgorithm() == ChartDecoding) {
      m_chartManager = new ChartManager(*m_sentence, &system);
      m_chartManager->ProcessSentence();
      ChartTrellisPathList nBestList;
      m_chartManager->CalcNBest(nBestSize, nBestList, distinctNbest);

      cerr << "generate nbest list " << filename << endl;
      cerr << "not implemented.." << endl;
      exit(1);
    	if (filename != "") {
    		ofstream out(filename.c_str());
    		if (!out) {
    			ostringstream msg;
    			msg << "Unable to open " << filename;
    			throw runtime_error(msg.str());
    		}
    		// TODO: handle sentence id (for now always 0)
//    		OutputNBestList(const ChartTrellisPathList &nBestList, const ChartHypothesis *bestHypo, const TranslationSystem* system, long translationId)
//    		OutputNBest(out, nBestList, StaticData::Instance().GetOutputFactorOrder(),m_manager->GetTranslationSystem(), 0);
    		out.close();
    	}
    	else {
//    		OutputNBest(streamOut, nBestList, StaticData::Instance().GetOutputFactorOrder(),m_manager->GetTranslationSystem(), sentenceid);
    	}
    }
    else {
    	// run the decoder
      m_manager = new Moses::Manager(*m_sentence, staticData.GetSearchAlgorithm(), &system);
      m_manager->ProcessSentence();
      TrellisPathList nBestList;
      m_manager->CalcNBest(nBestSize, nBestList, distinctNbest);

      if (filename != "") {
    		ofstream out(filename.c_str());
    		if (!out) {
    			ostringstream msg;
    			msg << "Unable to open " << filename;
    			throw runtime_error(msg.str());
    		}
    		// TODO: handle sentence id (for now always 0)
    		OutputNBest(out, nBestList, StaticData::Instance().GetOutputFactorOrder(),m_manager->GetTranslationSystem(), 0);
    		out.close();
      }
      else {
      	OutputNBest(streamOut, nBestList, StaticData::Instance().GetOutputFactorOrder(),m_manager->GetTranslationSystem(), sentenceid);
      }
    }
  }

  void MosesDecoder::initialize(StaticData& staticData, const std::string& source, size_t sentenceid,
      													float bleuObjectiveWeight, float bleuScoreWeight, bool avgRefLength) {
  	m_sentence = new Sentence();
    stringstream in(source + "\n");
    const std::vector<FactorType> &inputFactorOrder = staticData.GetInputFactorOrder();
    m_sentence->Read(in,inputFactorOrder);

    // set weight of BleuScoreFeature
    staticData.ReLoadBleuScoreFeatureParameter(bleuObjectiveWeight*bleuScoreWeight);

    m_bleuScoreFeature->SetCurrSourceLength((*m_sentence).GetSize());
    if (avgRefLength)
    	m_bleuScoreFeature->SetCurrAvgRefLength(sentenceid);
    else
    	m_bleuScoreFeature->SetCurrShortestRefLength(sentenceid);
    m_bleuScoreFeature->SetCurrReferenceNgrams(sentenceid);
  }

  float MosesDecoder::getBleuScore(const ScoreComponentCollection& scores) {
    return scores.GetScoreForProducer(m_bleuScoreFeature);
  }

  void MosesDecoder::setBleuScore(ScoreComponentCollection& scores, float bleu) {
    scores.Assign(m_bleuScoreFeature, bleu);
  }

  ScoreComponentCollection MosesDecoder::getWeights() {
    return StaticData::Instance().GetAllWeights();
  }

  void MosesDecoder::setWeights(const ScoreComponentCollection& weights) {
    StaticData::InstanceNonConst().SetAllWeights(weights);
  }

  void MosesDecoder::updateHistory(const vector<const Word*>& words) {
    m_bleuScoreFeature->UpdateHistory(words);
  }

  void MosesDecoder::updateHistory(const vector< vector< const Word*> >& words, vector<size_t>& sourceLengths, vector<size_t>& ref_ids, size_t rank, size_t epoch) {
	  m_bleuScoreFeature->UpdateHistory(words, sourceLengths, ref_ids, rank, epoch);
  }

  void MosesDecoder::printBleuFeatureHistory(std::ostream& out) {
  	m_bleuScoreFeature->PrintHistory(out);
  }

  size_t MosesDecoder::getClosestReferenceLength(size_t ref_id, int hypoLength) {
  	return m_bleuScoreFeature->GetClosestRefLength(ref_id, hypoLength);
  }

  size_t MosesDecoder::getShortestReferenceIndex(size_t ref_id) {
  	return m_bleuScoreFeature->GetShortestRefIndex(ref_id);
  }

  void MosesDecoder::setBleuParameters(bool sentenceBleu, bool scaleByInputLength, bool scaleByAvgInputLength,
		  bool scaleByInverseLength, bool scaleByAvgInverseLength,
		  float scaleByX, float historySmoothing, size_t scheme, float relax_BP,
		  bool useSourceLengthHistory) {
	  m_bleuScoreFeature->SetBleuParameters(sentenceBleu, scaleByInputLength, scaleByAvgInputLength,
			  scaleByInverseLength, scaleByAvgInverseLength,
			  scaleByX, historySmoothing, scheme, relax_BP,
			  useSourceLengthHistory);
  }
} 
