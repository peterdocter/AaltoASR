#include <algorithm>
#include <iomanip>

#include <math.h>
#include "Search.hh"

int HypoPath::count = 0;

void
HypoStack::prune_similar(int length)
{
  for (int h1 = 0; h1 + 1 < size(); h1++) {
    for (int h2 = h1 + 1; h2 < size(); h2++) {
      bool match = true;
      HypoPath *p1 = at(h1).path;
      HypoPath *p2 = at(h2).path;
      for (int i = 0; i < length; i++) {
	if (!p1 || !p2) {
	  match = false;
	  break;
	}

	if (p1->word_id != p2->word_id) {
	  match = false;
	  break;
	}
	p1 = p1->prev;
	p2 = p2->prev;
      }

      if (match) {
	erase(begin() + h2);
	h2--;
      }
    }
  }
}

Search::Search(Expander &expander, const Vocabulary &vocabulary, 
	       const Ngram &ngram)
  : m_expander(expander),
    m_vocabulary(vocabulary),
    m_ngram(ngram),

    // Stacks states
    m_first_frame(0),
    m_last_frame(0),
    m_first_stack(0),
    m_last_stack(0),

    // Options
    m_lm_scale(1),
    m_lm_offset(1),
    m_verbose(false),

    // Pruning options
    m_word_limit(0),
    m_word_beam(1e10),
    m_hypo_limit(0),
    m_beam(1e10),
    m_global_beam(1e10),

    // Temp
    m_history(0)
{
}

void
Search::debug_print_hypo(Hypo &hypo)
{
  static std::vector<HypoPath*> debug_paths(128);

  HypoPath *path = hypo.path;
  std::cout.setf(std::cout.fixed, std::cout.floatfield);
  std::cout.setf(std::cout.right, std::cout.adjustfield);
  std::cout.precision(2);
  std::cout << std::setw(5) << hypo.frame;
  std::cout << std::setw(10) << hypo.log_prob;

  debug_paths.clear();
  while (path != NULL) {
    debug_paths.push_back(path);
    path = path->prev;
  }

  for (int i = debug_paths.size() - 1; i >= 0; i--) {
    std::cout << " " 
	      << "<" << debug_paths[i]->frame << "> "
	      << "(" << debug_paths[i]->word_id << ")"
	      << m_vocabulary.word(debug_paths[i]->word_id);
  }
  std::cout << " <" << hypo.frame << ">" << std::endl;
}

void
Search::debug_print_history(Hypo &hypo)
{
  static std::vector<HypoPath*> paths(128);

  HypoPath *path = hypo.path;

  paths.clear();
  while (path != NULL) {
    paths.push_back(path);
    path = path->prev;
  }

  for (int i = paths.size() - 1; i > 0; i--) {
    std::cout << paths[i]->frame*128 << " " << paths[i-1]->frame*128
	      << " " << m_vocabulary.word(paths[i]->word_id) << std::endl;
  }
}

void
Search::init_search(int expand_window, int stacks, int reserved_hypos)
{
  m_frame = 0;
  m_expand_window = expand_window;

  // FIXME!  Are all beams reset properly here.  Test reinitializing
  // the search!
  m_global_best = 1e10;
  m_global_frame = -1;

  // Initialize stacks
  m_stacks.resize(stacks);
  m_first_frame = 0;
  m_last_frame = m_first_frame + stacks - 1;
  m_first_stack = 0;
  m_last_stack = m_first_stack + stacks - 1;

  // Clear stacks and reserve some space.  Stacks grow dynamically,
  // but if we need space anyway, it is better to have some already.
  for (int i = 0; i < m_stacks.size(); i++) {
    m_stacks[i].clear();
    m_stacks[i].reserve(reserved_hypos);
  }

  // Create initial empty hypothesis.
  Hypo hypo;
  m_stacks[0].add(hypo);
}

int
Search::frame2stack(int frame) const
{
  // Check that we have the frame in buffer
  if (frame < m_first_frame)
    throw ForgottenFrame();
  if (frame > m_last_frame)
    throw FutureFrame();

  // Find the stack corresponding to the given frame
  int index = frame - m_first_frame;
  index = (m_first_stack + index) % m_stacks.size();

  return index;
}

void
Search::sort_stack(int frame, int top)
{
  int stack_index = frame2stack(frame);
  HypoStack &stack = m_stacks[stack_index];
  stack.partial_sort(top);
}

void
Search::circulate(int &stack)
{
  stack++;
  if (stack >= m_stacks.size())
    stack = 0;
}

void
Search::move_buffer(int frame)
{
  while (m_last_frame < frame) {
    m_stacks[m_first_stack].clear();
    circulate(m_first_stack);
    circulate(m_last_stack);
    m_first_frame++;
    m_last_frame++;
  }
}

void
Search::prune_similar(int frame, int length)
{
  int stack_index = frame2stack(frame);
  HypoStack &stack = m_stacks[stack_index];
  stack.prune_similar(length);
}

bool
Search::expand(int frame)
{
  int stack_index = frame2stack(frame);
  HypoStack &stack = m_stacks[stack_index];

  // Prune stack
  if (m_hypo_limit > 0)
    stack.prune(m_hypo_limit);

  // Debug print
  if (m_verbose && !stack.empty()) {
    std::cout << HypoPath::count << " " << Lexicon::Path::count << " ";
    debug_print_hypo(stack[stack.best_index()]);
  }

  // FIXME?!
  // End of input?  Do not expand stack on the last existing frame?
  // Is there a stack at the first non-existing frame?
  if (m_expander.eof_frame() >= 0 &&
      frame >= m_expander.eof_frame() - 3)
    return false;
      
  // Reset global pruning if current stack is best
  if (m_global_frame == frame) {
    m_global_best = 1e10;
    m_global_frame = -1;
  }

  // Expand all hypotheses in the stack, but only if inside the
  // global_beam
  double angle = m_global_best / m_global_frame;
  double ref = m_global_best + angle * (frame - m_global_frame);
  if (!stack.empty() &&
      stack.best_log_prob() > ref - m_global_beam) {
    // Fit word lexicon to acoustic data
    m_expander.expand(frame, m_expand_window);

    // Get only the best words
    // FIXME: perhaps we want to add LM probs here
    std::vector<Expander::Word*> words = m_expander.words();
    if (m_word_limit > 0 && words.size() > m_word_limit) {
      std::partial_sort(words.begin(), words.begin() + m_word_limit, 
			words.end(),
			Expander::WordCompare());
      words.resize(m_word_limit);
    }

    // Expand all hypotheses in the stack...
    for (int h = 0; h < stack.size(); h++) {
      Hypo &hypo = stack[h];

      // Only hypotheses inside the beam
      if (hypo.log_prob < stack.best_log_prob() - m_beam)
	continue;

      // ... Using the best words
      for (int w = 0; w < words.size(); w++) {
	Expander::Word *word = words[w];

	// Prune words much worse than the best words on average
	if (word->avg_log_prob < words[0]->avg_log_prob * m_word_beam)
	  continue;

	double log_prob = hypo.log_prob + word->log_prob;
	  
	// Calculate language model probabilities
	if (m_ngram.order() > 0) {
	  m_history.clear();
	  m_history.push_front(word->word_id);
	  HypoPath *path = hypo.path;
	  for (int i = 0; i < m_ngram.order(); i++) {
	    if (!path)
	      break;
	    m_history.push_front(path->word_id);
	    path = path->prev;
	  }

	  log_prob += m_lm_offset + m_lm_scale * word->frames *
	    m_ngram.log_prob(m_history.begin(), m_history.end());
	}

	// Ensure stack space
	move_buffer(frame + word->frames);
	assert(frame >= m_first_frame);
	int index = frame2stack(frame + word->frames);
	HypoStack &target_stack = m_stacks[index];

	// Insert hypo to target stack, if inside the beam
	if (log_prob > target_stack.best_log_prob() - m_beam) {
	  int target_frame = frame + word->frames;
	  Hypo new_hypo(target_frame, log_prob, hypo.path);
	  new_hypo.add_path(word->word_id, hypo.frame);
	  target_stack.add(new_hypo);

	  // Update global pruning
	  double avg_log_prob = log_prob / target_frame;
	  if (avg_log_prob > m_global_best / m_global_frame) {
	    m_global_best = log_prob;
	    m_global_frame = target_frame;
	  }
	}
      }
    }
  }

  // FIXME REALLY: is this good idea?!  
  if (m_expander.words().size() > 0)
    stack.clear();

  return true;
}

void
Search::go(int frame)
{
  while (m_frame < frame) {
    int index = frame2stack(m_frame);
    HypoStack &stack = m_stacks[index];
    stack.clear();
    m_frame++;
  }
}

bool
Search::run()
{
  if (!expand(m_frame))
    return false;
  m_frame++;
  return true;
}
