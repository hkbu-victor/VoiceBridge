/*
	Copyright 2017-present Zoltan Somogyi (AI-TOOLKIT), All Rights Reserved
	You may use this file only if you agree to the software license:
	AI-TOOLKIT Open Source Software License - Version 2.0 - January 9, 2018:
	https://ai-toolkit.blogspot.com/p/ai-toolkit-open-source-software-license.html.
	Also included with the source code distribution in AI-TOOLKIT-LICENSE.txt.

	Based on :	
	Apache 2.0.
	Copyright  2014  Johns Hopkins University (author: Daniel Povey)
			   2014  Guoguo Chen
			   2015  Hainan Xu
*/

#include "kaldi-win\scr\kaldi_scr.h"

/*
	DictDirAddPronprobs()
	This function takes pronunciation counts, e.g. generated by aligning your training data and getting the prons using 
	steps/get_prons, and creates a modified dictionary directory with pronunciation probabilities. If the [input-sil-counts]
	parameter is provided, it will also include silprobs in the generated lexicon.	
*/
int DictDirAddPronprobs(
	fs::path srcdir,		//input-dict-dir
	fs::path dir,			//output-dict-dir	
	fs::path pron_counts,	//input-pron-counts
	fs::path sil_counts,	//input-sil-counts
	fs::path bigram_counts,	//input-bigram-counts
	bool max_normalize		//If true, divide each pron-prob by the most likely pron-prob per word. Default=true
	)
{
	if (!fs::exists(pron_counts) || fs::is_empty(pron_counts)) {
		LOGTW_ERROR << "Expected " << pron_counts.string() << " to exist.";
		return -1;
	}

	try	{
		fs::create_directory(dir);
	} catch (const std::exception&)	{
		LOGTW_ERROR << "Failed to create " << dir.string();
		return -1;
	}

	if (ValidateDict(srcdir) < 0) return -1;

	//we read in the lexicon which also removes any multiple delimiters and then sort 
	//on the first field and save to the new dict dir
	fs::path src_lex;
	StringTable t_lex, t_pronc;
	if (fs::exists(srcdir / "lexicon.txt") && !fs::is_empty(srcdir / "lexicon.txt"))
		src_lex = srcdir / "lexicon.txt";	
	else if (fs::exists(srcdir / "lexiconp.txt") && !fs::is_empty(srcdir / "lexiconp.txt"))
		src_lex = srcdir / "lexiconp.txt";
	if (ReadStringTable(src_lex.string(), t_lex) < 0) return -1;
	SortStringTable(t_lex, 0, 0);
	fs::ofstream ofs(dir / "lexicon.txt", fs::ofstream::binary | fs::ofstream::out);
	if (!ofs) {
		LOGTW_ERROR << "Failed to write to file " << (dir / "lexicon.txt").string();
		return -1;
	}
	for (string_vec & _s : t_lex)	{
		if (src_lex == srcdir / "lexiconp.txt") {
			//remove field 2 (pron-probs)
			_s.erase(_s.begin() + 1);
		}
		std::string s = join_vector(_s, " ");
		ofs << s << '\n';
	}
	ofs.flush(); ofs.close();

	//combine the pron counts and the lexicon and apply add-one smoothing to the pron-probs
	t_lex.clear();
	if (ReadStringTable((dir / "lexicon.txt").string(), t_lex) < 0) return -1;
	int n_old = t_lex.size();
	if (ReadStringTable(pron_counts.string(), t_pronc) < 0) return -1;
	//make t_lex compatible with t_pronc: add an extra first column with 1
	for (string_vec & _s : t_lex)
		_s.insert(_s.begin(), "1");
	//merge the two tables
	t_lex.insert(std::end(t_lex), std::begin(t_pronc), std::end(t_pronc));
	//count words and prons
	using UMAPSI = std::unordered_map<std::string, int>;
	UMAPSI word_count, pron_count;
	std::map<std::string, std::string> pron2word;
	for (string_vec & _s : t_lex)
	{
		//save count
		int count = StringToNumber<int>(_s[0], -1);
		if (count < 1) {
			LOGTW_ERROR << "Syntax error in file " << (dir / "lexicon.txt").string();
			return -1;
		}
		//remove count from the vector
		_s.erase(_s.begin());
		//word count
		std::string word(_s[0]);
		UMAPSI::iterator ium = word_count.find(word);
		if (ium == word_count.end())
			word_count.emplace(word, count);
		else ium->second += count;
		//pron count
		//NOTE: we add the word also to the pronunciation because there can be several words with the same pronunciation!
		std::string pron = join_vector(_s, " ");
		UMAPSI::iterator ium1 = pron_count.find(pron);
		if (ium1 == pron_count.end())
			pron_count.emplace(pron, count);
		else ium1->second += count;
		//map
		pron2word[pron] = word;
	}

	//
	fs::ofstream ofs1(dir / "lexicon.temp", fs::ofstream::binary | fs::ofstream::out);
	if (!ofs1) {
		LOGTW_ERROR << "Failed to write to file " << (dir / "lexicon.temp").string();
		return -1;
	}
	for (auto & pair : pron_count)
	{
		std::string word = pron2word[pair.first];
		int num = pair.second;
		int den = word_count[word];
		ofs1 << (1.0 * num / den) << " " << pair.first << '\n';
	}

	ofs1.flush(); ofs1.close();
	StringTable t;
	if (ReadStringTable((dir / "lexicon.temp").string(), t) < 0) return -1;
	SortStringTable(t, 1, 0, "string", "number"); //TODO:... sort on 3rd field ?

	fs::ofstream ofs2(dir / "lexiconp.txt", fs::ofstream::binary | fs::ofstream::out);
	if (!ofs2) {
		LOGTW_ERROR << "Failed to write to file " << (dir / "lexiconp.txt").string();
		return -1;
	}
	static const boost::regex regex("^<eps>");
	boost::match_results<std::string::const_iterator> match_results;
	boost::match_flag_type flags = boost::match_default;
	int n_new = 0;
	using UMAPSD = std::unordered_map<std::string, double>;
	UMAPSD maxp;
	for (string_vec & _s : t)
	{		
		std::swap(_s[0], _s[1]);
		if (!boost::regex_search(_s[0], match_results, regex, flags))
		{
			ofs2 << join_vector(_s, " ") << '\n';
			n_new++;
		}

		if (max_normalize) {
			//normalizing pronprobs so maximum is 1 for each word.
			std::string word = _s[0];
			double pronprob = StringToNumber<double>(_s[1], -1);
			if (pronprob < 1) {
				LOGTW_ERROR << "Syntax error in file " << (dir / "lexiconp.txt").string();
				return -1;
			}
			UMAPSD::iterator ium = maxp.find(word);
			if (ium == maxp.end())
				maxp.emplace(word, pronprob);
			else {
				if(pronprob > ium->second) ium->second = pronprob;
			}
		}
	}
	ofs2.flush(); ofs2.close();

	//cleanup
	DeleteAllMatching(dir, boost::regex(".*(\\.temp)$"));

	if (n_old != n_new) {
		LOGTW_ERROR << "Number of lines differs from dir/lexicon.txt n_old vs dir/lexiconp.txt n_new";
		LOGTW_ERROR << "  " << dir.string();
		LOGTW_ERROR << "  Probably something went wrong (e.g. input prons were generated from a different lexicon";
		LOGTW_ERROR << "  than srcdir, or you used pron_counts.txt when you should have used pron_counts_nowb.txt";
		LOGTW_ERROR << "  or something else. Make sure the prons in src_lex pron_counts look the same.";
		LOGTW_ERROR << "  srcdir: " << srcdir.string();
		LOGTW_ERROR << "  src_lex: " << src_lex.string();
		return -1;
	}

	if (max_normalize) {
		t.clear();
		if (ReadStringTable((dir / "lexiconp.txt").string(), t) < 0) return -1;
		fs::ofstream ofs3(dir / "lexiconp.txt", fs::ofstream::binary | fs::ofstream::out);
		if (!ofs3) {
			LOGTW_ERROR << "Failed to write to file " << (dir / "lexiconp.txt").string();
			return -1;
		}
		for (string_vec & _s : t)
		{
			double pronprob = StringToNumber<double>(_s[1], -1.0);
			UMAPSD::iterator ium = maxp.find(_s[0]);
			if (ium == maxp.end()) {
				LOGTW_ERROR << "Syntax error in file " << (dir / "lexiconp.txt").string();
				return -1;
			}
			else {
				pronprob /= ium->second;
				_s[1] = std::to_string(pronprob);
			}
			ofs3 << join_vector(_s, " ") << '\n';
		}
		ofs3.flush(); ofs3.close();
	}

	/*
	 Create dir/lexiconp_silprob.txt and dir/silprob.txt if silence counts file
	 exists. The format of dir/lexiconp_silprob.txt is: 
	 word pron-prob P(s_r | w)  F(s_l | w) F(n_l | w) pron
	*/


	//TODO:... low pripority because it has a very small effect on the WER.


	LOGTW_INFO << "\n\n";
	LOGTW_INFO << "************  this functionality is still under construction *************** ";
	LOGTW_INFO << "\n\n";









	return 0;
}

//USAGE:
/*
Compute the pronunciation and silence probabilities from training data, and re-create the lang directory.
NOTE: depending on if LDA+MLLT+SAT or DELTA+SAT model is used the dir folder will be different.

NOTE: using silence and pronunciation probabilities have only a very small effect on the acccuracy (WER).
*/
/*
int TestLibriSpeech_PronSilProb(std::vector<std::string> lms, fs::path dir)
{
fs::path training_dir(voicebridgeParams.pth_data / voicebridgeParams.train_base_name);
fs::path test_dir(voicebridgeParams.pth_data / voicebridgeParams.test_base_name);

int numthreads = concurentThreadsSupported;
if (numthreads < 1) numthreads = 1;

int ret = GetProns(training_dir, voicebridgeParams.pth_lang, dir);
if (ret < 0)
{
LOGTW_ERROR << "Failed getting prons.";
return -1;
}

fs::path dict_sp(voicebridgeParams.pth_dict.string() + "_sp");
ret = DictDirAddPronprobs(
voicebridgeParams.pth_dict,		//input dict
dict_sp,						//output dict with sil probs
training_dir / "tri3c" / "pron_counts_nowb.txt",
training_dir / "tri3c" / "sil_counts_nowb.txt",
training_dir / "tri3c" / "pron_bigram_counts_nowb.txt",
true);
if (ret < 0)
{
LOGTW_ERROR << "Failed adding sil probs to dictionary.";
return -1;
}

LOGTW_INFO << "\n\n";
LOGTW_INFO << "************************************";
LOGTW_INFO << "****   Pron and Sil Probs OK!   ****";
LOGTW_INFO << "************************************";

return 0;
}
*/