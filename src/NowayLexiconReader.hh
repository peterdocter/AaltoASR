#ifndef NOWAYLEXICONREADER_HH
#define NOWAYLEXICONREADER_HH

#include <iostream>
#include <map>
#include <string>

#include "Lexicon.hh"
#include "Vocabulary.hh"
#include "Hmm.hh"

// A class that creates a lexicon from a dictionary file
//
// NOTES:
//
// Even after exception in reading, the read can be continued.
//
// FILE FORMAT:
//
// ^word(prob) phone1 phone2 ...
// ^word phone1 phone2 ...
//
// LIMITS:
//
// Maximum line length is limited, but at least 4096.

class NowayLexiconReader {
public:
  NowayLexiconReader(const std::map<std::string,int> &hmm_map, 
		     const std::vector<Hmm> &hmms);

  void read(std::istream &in);
  Lexicon &lexicon() { return m_lexicon; }
  Vocabulary &vocabulary() { return m_vocabulary; }

  struct ReadError : public std::exception {
    virtual const char *what() const throw()
      { return "NowayLexiconReader: read error"; }
  };

  struct InvalidProbability : public std::exception {
    virtual const char *what() const throw() 
      { return "NowayLexiconReader: invalid probability"; }
  };

  struct UnknownHmm : public std::exception {
    virtual const char *what() const throw()
      { return "NowayLexiconReader: unknown hmm"; }
  };

protected:
  const std::map<std::string,int> &m_hmm_map;
  const std::vector<Hmm> &m_hmms;
  Lexicon m_lexicon;
  Vocabulary m_vocabulary;
  int m_line_no;
};

#endif /* NOWAYLEXICONREADER_HH */
