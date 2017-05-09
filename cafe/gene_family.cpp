#include <sstream>
#include <fstream>
#include <algorithm>
#include <iostream>

#include "gene_family.h"
#include <tree.h>

extern "C" {
#include "cafe.h"
extern pCafeParam cafe_param;
void __cafe_famliy_check_the_pattern(pCafeFamily pcf);
}

using namespace std;

class my_ctype : public std::ctype<char> {
  mask my_table[table_size + 1];
public:
  my_ctype(size_t refs = 0)
    : std::ctype<char>(my_table, false, refs)
  {
    // copy the original character classifaction table.
    std::copy(classic_table(),
      classic_table() + table_size,
      my_table);
    // and change ',' to be classed as white space.
    my_table[','] = (mask)space;
  }
};

vector<string> tokenize(string s, int flags)
{
  istringstream iss(s);
  if (flags & COMMA_AS_WHITESPACE)
  {
    std::locale x(std::locale::classic(), new my_ctype);
    iss.imbue(x);
  }
  vector<string> result;

  while (iss.good()) {
    string tmp;
    iss >> tmp;
    if (tmp.size() > 0)
      result.push_back(tmp);
  }

  return result;
}

vector<string> string_split(string line, char delimiter)
{
  vector<string> result;
  stringstream strstr(line);
  string word = "";
  while (getline(strstr, word, delimiter)) 
    result.push_back(word);
  return result;
}

pCafeFamily cafe_family_init(const std::vector<std::string>& species_list)
{
  pCafeFamily pcf = (pCafeFamily)memory_new(1, sizeof(CafeFamily));
  pcf->num_species = species_list.size();
  pcf->max_size = 0;
  pcf->flist = arraylist_new(11000);

  pcf->species = (char**)memory_new(species_list.size(), sizeof(char*));
  for (size_t i = 0; i < species_list.size(); i++)
  {
    pcf->species[i] = new char[species_list[i].size()+1];
    strcpy(pcf->species[i], species_list[i].c_str());
  }

  pcf->index = (int*)memory_new(pcf->num_species, sizeof(int));
  for (int i = 0; i < pcf->num_species; ++i)
  {
    pcf->index[i] = -1;
  }

  return pcf;
}

void cafe_family_free(pCafeFamily pcf)
{
  for (int i = 0; i < pcf->num_species; i++) 
  { 
    delete [] pcf->species[i];  
    pcf->species[i] = NULL;
  }
  memory_free(pcf->species);
  pcf->species = NULL;
  memory_free(pcf->index);
  pcf->index = NULL;
  arraylist_free(pcf->flist, (freefunc)cafe_family_item_free);
  memory_free(pcf);
  pcf = NULL;
}

pCafeFamily load_gene_families(std::istream& ist, int bpatcheck, char separator)
{
  char buf[STRING_BUF_SIZE];
  if (!ist)
  {
    return NULL;
  }

  ist.getline(buf, STRING_BUF_SIZE);
  string_pchar_chomp(buf);

  vector<string> species_list = string_split(buf, separator);
  if (species_list.size() < 2)
    throw runtime_error("Failed to identify species for gene families");

  species_list.erase(species_list.begin(), species_list.begin()+2); // first two items are description and ID - delete them
  pCafeFamily pcf = cafe_family_init(species_list);

  for (int i = 0; ist.getline(buf, STRING_BUF_SIZE); i++)
  {
    if (!ist)
      break;

    cafe_family_add_item(pcf, string_split(buf, separator));
  }
  if (bpatcheck)
  {
    __cafe_famliy_check_the_pattern(pcf);
  }
  return pcf;
}

/// Data array is assumes to contain a description, an identifier, and a set of integers
/// giving the family size in species order
void cafe_family_add_item(pCafeFamily pcf, const vector<string>& data)
{
  pCafeFamilyItem pitem = (pCafeFamilyItem)memory_new(1, sizeof(CafeFamilyItem));
  pitem->count = (int*)calloc(pcf->num_species, sizeof(int));
  pitem->maxlh = -1;   // Maximum likelihood index
  pitem->ref = -1;
  pitem->lambda = NULL;
  pitem->mu = NULL;
  pitem->holder = 1;

  if (data.size() != size_t(pcf->num_species + 2))
  {
    std::cerr << "Inconsistency in column count: expected " << pcf->num_species + 2 << ", but found " << data.size();
  }

  pitem->desc = new char[data[0].size()+1];
  strcpy(pitem->desc, data[0].c_str());
  pitem->id = new char[data[1].size() + 1];
  strcpy(pitem->id, data[1].c_str());

  for (int j = 0; j < pcf->num_species; j++)
  {
    if (data[j + 2].empty())
    {
      pitem->count[j] = 0;
    }
    else
    {
      pitem->count[j] = atoi(data[j + 2].c_str());
    }
  }
  pcf->max_size = max(pcf->max_size, *std::max_element(pitem->count, pitem->count + pcf->num_species));
  arraylist_add(pcf->flist, pitem);
}

void cafe_family_item_free(pCafeFamilyItem pitem)
{
  delete [] pitem->id;
  pitem->id = NULL;
  memory_free(pitem->count);
  pitem->count = NULL;
  delete[] pitem->desc;
  pitem->desc = NULL;
  if (pitem->holder)
  {
    if (pitem->lambda) { memory_free(pitem->lambda); pitem->lambda = NULL; }
    if (pitem->mu) { memory_free(pitem->mu); pitem->mu = NULL; }
  }
  memory_free(pitem);
  pitem = NULL;
}


double cross_validate_by_family(const char* queryfile, const char* truthfile, const char* errortype)
{
  int i, j;
  double MSE = 0;
  double MAE = 0;
  double SSE = 0;
  double SAE = 0;
  cafe_family_read_query_family(cafe_param, queryfile);
  if (cafe_param->cv_test_count_list == NULL) return -1;

  // read in validation data
  ifstream ifst(truthfile);
  pCafeFamily truthfamily = load_gene_families(ifst, 1, '\t');
  if (truthfamily == NULL) {
    fprintf(stderr, "failed to read in true values %s\n", truthfile);
    return -1;
  }

  // now compare reconstructed count to true count	
  pCafeTree pcafe = cafe_param->pcafe;
  pCafeTree truthtree = cafe_tree_copy(pcafe);
  // set parameters
  if (truthtree)
  {
    cafe_family_set_species_index(truthfamily, truthtree);
  }

  reset_birthdeath_cache(cafe_param->pcafe, cafe_param->parameterized_k_value, &cafe_param->family_size);

  for (i = 0; i< cafe_param->cv_test_count_list->size; i++)
  {
    int* testcnt = (int*)cafe_param->cv_test_count_list->array[i];
    cafe_family_set_size(truthfamily, i, truthtree);
    cafe_family_set_size_by_species((char *)cafe_param->cv_test_species_list->array[i], *testcnt, pcafe);
    if (cafe_param->posterior) {
      cafe_tree_viterbi_posterior(pcafe, cafe_param);
    }
    else {
      cafe_tree_viterbi(pcafe);
    }
    // leaf nodes SSE
    SSE = 0;
    SAE = 0;
    int nodecnt = 0;
    for (j = 0; j<pcafe->super.nlist->size; j = j + 2) {
      int error = ((pCafeNode)truthtree->super.nlist->array[j])->familysize - ((pCafeNode)pcafe->super.nlist->array[j])->familysize;
      SSE += pow(error, 2);
      SAE += abs(error);
      nodecnt++;
    }
    MSE += SSE / nodecnt;
    MSE += SAE / nodecnt;
  }
  cafe_free_birthdeath_cache(pcafe);

  MSE = MSE / cafe_param->cv_test_count_list->size;
  MAE = MAE / cafe_param->cv_test_count_list->size;
  cafe_log(cafe_param, "MSE %f\n", MSE);
  cafe_log(cafe_param, "MAE %f\n", MSE);

  double returnerror = -1;
  if (strncmp(errortype, "MSE", 3) == 0) {
    returnerror = MSE;
  }
  else if (strncmp(errortype, "MAE", 3) == 0) {
    returnerror = MAE;
  }
  return returnerror;
}

void cafe_family_filter(pCafeParam param)
{
  int i, n;
  pCafeFamily pcf = param->pfamily;
  pCafeTree pcafe = param->pcafe;
  pArrayList fflist = arraylist_new(11000);
  int max = 0;
  for (i = 0; i < pcf->flist->size; i++)
  {
    cafe_family_set_size(pcf, i, pcafe);
    pArrayList nlist = pcafe->super.nlist;
    for (n = 0; n < nlist->size; n += 2)
    {
      pCafeNode pnode = (pCafeNode)nlist->array[n];
      if (pnode->familysize > 0)
      {
        pTreeNode p = ((pTreeNode)pnode);
        do
        {
          p->reg = 1;
          p = p->parent;
        } while (p && p->reg == 0);
      }
    }
    pTreeNode root = ((pTree)pcafe)->root;
    pTreeNode child[2] = { (pTreeNode)tree_get_child(root,0), (pTreeNode)tree_get_child(root,1) };

    root->reg = 0;
    if (child[0]->reg && child[1]->reg)
    {
      root->reg = 1;
    }

    // (1 and 1)
    else if (child[0]->reg || child[1]->reg)
    {
      root->reg = 0;
    }

    pCafeFamilyItem pitem = (pCafeFamilyItem)pcf->flist->array[i];
    if (root->reg == 1)
    {
      arraylist_add(fflist, pitem);
      for (n = 0; n < pcf->num_species; n++)
      {
        if (pitem->count[n] > max)
        {
          max = pitem->count[n];
        }
      }
    }
    else
    {
      cafe_family_item_free(pitem);
    }
    tree_clear_reg((pTree)pcafe);
  }
  if (pcf->flist->size != fflist->size)
  {
    cafe_log(param, "The Number of families : %d ==> %d\n", pcf->flist->size, fflist->size);
    arraylist_free(pcf->flist, NULL);
    pcf->flist = fflist;
    for (i = 0; i < fflist->size; i++)
    {
      pCafeFamilyItem pitem = (pCafeFamilyItem)pcf->flist->array[i];
      pitem->ref = -1;
    }
    __cafe_famliy_check_the_pattern(pcf);
  }
  else
  {
    arraylist_free(fflist, NULL);
  }

  if (pcf->max_size != max)
  {
    pcf->max_size = max;
    init_family_size(&param->family_size, max);
    cafe_tree_set_parameters(pcafe, &param->family_size, 0);
  }
}

