// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
// 1. Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
// 3. Neither the name of Jeremy Salwen nor the name of any other
//    contributor may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY Jeremy Salwen AND CONTRIBUTORS ``AS IS'' AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL Jeremy Salwen OR ANY OTHER
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, 
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
// OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
// WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
// OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
// ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#include <iostream>
#include <fstream>
#include <cstdio>
#include <algorithm>
#include <numeric>
#include <sstream>

#include <sys/resource.h>

#include <boost/filesystem.hpp>
#include <boost/circular_buffer.hpp>
#include <boost/unordered_map.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/program_options.hpp>

#include "common.hpp"


namespace po=boost::program_options;

FILE* open_file(int midid, const std::string& outdir) {
	std::ostringstream s;
	s<<outdir<<"/"<< midid << ".vectors";
	return fopen(s.str().c_str(),"w");
}

void compute_and_output_context(const boost::circular_buffer<int>& context, const std::vector<float>& idfs,
                                const arma::fmat& origvects, std::vector<FILE*>& outfiles, std::vector<size_t>& found,
                                unsigned int vecdim, unsigned int contextsize, int prune, int from, unsigned int max_contexts) {
	int midid=context[contextsize]; 
	int fileid = midid - from;
	if (midid < from || midid >= prune + from) {
		return;
	}
	if (max_contexts > 0 && ((midid > 0 && found[fileid] >= max_contexts) || found[fileid] >= 2 * max_contexts)) {
		return;
	}
	arma::fvec out(vecdim,arma::fill::zeros);
	compute_context(context, idfs,origvects,out,vecdim,contextsize);

	//now tot will contain the context representation of the middle vector
	fwrite(out.memptr(),sizeof(float),vecdim,outfiles[fileid]);
	found[fileid]++;
	if (found[fileid] % 1000 == 0) {
	//	fclose(outfiles[midid]);
		fflush(outfiles[fileid]);
	}
	if ((found[fileid] == max_contexts && midid > 0) || found[fileid] == 2 * max_contexts) {
		fclose(outfiles[fileid]);
		std::cout << "Finished word " << midid << std::endl;
	}
}

int extract_contexts(std::ifstream& vocabstream, std::ifstream& tfidfstream, std::ifstream& vectorstream, std::string indir, std::string outdir, int vecdim,unsigned int contextsize,std::string eodmarker, bool indexed, unsigned int prune, unsigned int from, unsigned int max_contexts) {
	boost::unordered_map<std::string, int> vocabmap;
	std::vector<std::string> vocab;
	std::vector<float> idfs;
	arma::fmat origvects(vecdim,5);
	
	std::string word;
	unsigned int index=0;
	while(getline(vocabstream,word)) {
		vocab.push_back(word);
		vocabmap[word]=index;

		float tf;
		tfidfstream >>tf;
		idfs.push_back(tf);

		if(index>=origvects.n_cols) {
			origvects.resize(origvects.n_rows,origvects.n_cols*2);
		}
		
		for(int i=0; i<vecdim; i++) {
			vectorstream>>origvects(i,index);
		}
		index++;
	}

	vocabstream.close();
	tfidfstream.close();
	vectorstream.close();


	int startdoci=lookup_word(vocabmap,"<s>",false);
	int enddoci=lookup_word(vocabmap,"<\\s>",false);

	unsigned int vsize=vocab.size();

        vsize -= from;
	if(prune && prune <vsize) {
		vsize=prune;
	}
	//set limit of open files high enough to open a file for every word in the dictionary.
	rlimit lim;
	lim.rlim_cur=vsize+1024; //1024 extra files just to be safe;
	lim.rlim_max=vsize+1024;
	setrlimit(RLIMIT_NOFILE , &lim);
	
	std::vector<FILE*> outfiles(vsize);
        std::vector<size_t> found(vsize);
	for(unsigned int i=0; i<vsize; i++) {
		std::ostringstream s;
		s<<outdir<<"/"<< from + i << ".vectors";
		outfiles[i]=fopen(s.str().c_str(),"w");
		if(outfiles[i]==NULL) {
			std::cerr<<"Error opening file "<<s.str() << std::endl;
			return 9;
		}
	}

	for (boost::filesystem::directory_iterator itr(indir); itr!=boost::filesystem::directory_iterator(); ++itr) {
		std::string path=itr->path().string();
		if(!boost::algorithm::ends_with(path,".txt")) {
			continue;
		}

		std::ifstream corpusreader(path.c_str());
		if(!corpusreader.good()) {
			return 7;
		}

		std::cout << "Reading corpus file " << path << std::endl;

		//Keeps track of the accumulated contexts of the previous 5 words, the current word, and the next 5 words
		boost::circular_buffer<int > context(2*contextsize+1);
		size_t documents = 0;

		do {
			context.clear();
			for(unsigned int i=0; i<contextsize; i++) {
				context.push_back(startdoci);
			}
			std::string word;
			for(unsigned int i=0; i<contextsize+1; i++) {
				if(getline(corpusreader,word)) {
					if(word==eodmarker) goto EOD;
					int wind=lookup_word(vocabmap,word,indexed);
					context.push_back(wind);
				}
			}

			while(getline(corpusreader,word)) {
				if(word==eodmarker) goto EOD;
				compute_and_output_context(context, idfs, origvects,outfiles, found, vecdim,contextsize,vsize, from, max_contexts);
				context.pop_front();
				int newind=lookup_word(vocabmap,word,indexed);
				context.push_back(newind);
			}
			EOD:
			unsigned int k=0;
			while(context.size()<2*contextsize+1) {
				context.push_back(enddoci);
				k++;
			}
			for(; k<contextsize; k++) {
				compute_and_output_context(context, idfs, origvects,outfiles, found, vecdim,contextsize,vsize, from, max_contexts);
				context.pop_front();
				context.push_back(enddoci);
			}
			documents++;
			if (documents % 100000 == 0 && documents > 0) {
				std::cout << documents << " documents processed." << std::endl;
			}
		} while(!corpusreader.eof());
	}
	std::cout << "Closing files" <<std::endl;
	/*
	// Not necessary, since exiting properly will clean up anyway (And is much faster)
	for(unsigned int i=0; i<vsize; i++) {
		if(i%1000==0) std::cout<< "Closing file " << i <<std::endl;
		fclose(outfiles[i]);
	}
	*/
	
	return 0;
}


int main(int argc, char** argv) {

	std::string vocabf;
	std::string idff;
	std::string vecf;
	std::string corpusd;
	std::string outd;
	int dim;
	unsigned int contextsize;
	std::string eod;
	unsigned int prune=0;
	unsigned int from=0;
	unsigned int max_contexts=0;
	po::options_description desc("CExtractContexts Options");
	desc.add_options()
	("help,h", "produce help message")
	("vocab,v", po::value<std::string>(&vocabf)->value_name("<filename>")->required(), "vocab file")
	("idf,i", po::value<std::string>(&idff)->value_name("<filename>")->required(), "idf file")
	("vec,w", po::value<std::string>(&vecf)->value_name("<filename>")->required(), "word vectors file")
	("corpus,c", po::value<std::string>(&corpusd)->value_name("<directory>")->required(), "corpus directory")
	("outdir,o", po::value<std::string>(&outd)->value_name("<directory>")->required(), "directory to output contexts")
	("dim,d", po::value<int>(&dim)->value_name("<number>")->default_value(50),"word vector dimension")
	("contextsize,s", po::value<unsigned int>(&contextsize)->value_name("<number>")->default_value(5),"size of context (# of words before and after)")
	("eodmarker,e",po::value<std::string>(&eod)->value_name("<string>")->default_value("eeeoddd"),"end of document marker")
	("preindexed","indicates the corpus is pre-indexed with the vocab file")
	("prune,p",po::value<unsigned int>(&prune)->value_name("<number>"),"only output contexts for the first N words in the vocab")
	("from,f",po::value<unsigned int>(&from)->value_name("<number>"),"only output words from this index (from + prune = last)")
	("max-contexts,m",po::value<unsigned int>(&max_contexts)->value_name("<number>"),"stop collecting contexts for a word once this count is reached. 0 = no limit") ;
	
    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);

	if (vm.count("help")) {
		std::cout << desc << "\n";
		return 0;
	}

	try {
		po::notify(vm);
	} catch(po::required_option& exception) {
		std::cerr << "Error: " << exception.what() << "\n";
		std::cout << desc << "\n";
        return 1;
	}
	
	std::ifstream vocab(vocabf);
	if(!vocab.good()) {
		std::cerr << "Vocab file no good" <<std::endl;
		return 2;
	}

	std::ifstream frequencies(idff);
	if(!frequencies.good()) {
		std::cerr << "Frequencies file no good" <<std::endl;
		return 3;
	}

	std::ifstream vectors(vecf);
	if(!vectors.good()) {
		std::cerr << "Vectors file no good" <<std::endl;
		return 4;
	}


	if(!boost::filesystem::is_directory(corpusd)) {
		std::cerr << "Input directory does not exist" <<std::endl;
		return 5;
	}

	if(!boost::filesystem::is_directory(outd)) {
		std::cerr << "Input directory does not exist" <<std::endl;
		return 6;
	}

	return extract_contexts(vocab, frequencies, vectors, corpusd, outd, dim, contextsize,
                            eod, vm.count("preindexed")>0,prune, from, max_contexts);
}


