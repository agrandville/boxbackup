// --------------------------------------------------------------------------
//
// File
//		Name:    ExcludeList.cpp
//		Purpose: General purpose exclusion list
//		Created: 28/1/04
//
// --------------------------------------------------------------------------

#include "Box.h"

#ifdef WIN32
    #include <boost/regex.hpp>
#else
#ifndef PLATFORM_REGEX_NOT_SUPPORTED
	#include <regex.h>
	#define EXCLUDELIST_IMPLEMENTATION_REGEX_T_DEFINED
#endif // PLATFORM_REGEX_NOT_SUPPORTED
#endif // WIN32

#include "ExcludeList.h"
#include "Utils.h"
#include "Configuration.h"

#include "MemLeakFindOn.h"

// --------------------------------------------------------------------------
//
// Function
//		Name:    ExcludeList::ExcludeList()
//		Purpose: Constructor. Generates an exclude list which will allow everything
//		Created: 28/1/04
//
// --------------------------------------------------------------------------
ExcludeList::ExcludeList()
	: mpAlwaysInclude(0)
{
}


// --------------------------------------------------------------------------
//
// Function
//		Name:    ExcludeList::~ExcludeList()
//		Purpose: Destructor
//		Created: 28/1/04
//
// --------------------------------------------------------------------------
ExcludeList::~ExcludeList()
{
#ifdef WIN32
	// under win32 and boost - 
	// we didn't use pointers so all should auto destruct.
#else // ! WIN32
#ifndef PLATFORM_REGEX_NOT_SUPPORTED
	// free regex memory
	while(mRegex.size() > 0)
	{
		regex_t *pregex = mRegex.back();
		mRegex.pop_back();
		// Free regex storage, and the structure itself
		::regfree(pregex);
		delete pregex;
	}
#endif // ! PLATFORM_REGEX_NOT_SUPPORTED
#endif // WIN32

	// Clean up exceptions list
	if(mpAlwaysInclude != 0)
	{
		delete mpAlwaysInclude;
		mpAlwaysInclude = 0;
	}
}


// --------------------------------------------------------------------------
//
// Function
//		Name:    ExcludeList::AddDefiniteEntries(const std::string &)
//		Purpose: Adds a number of definite entries to the exclude list -- ones which
//				 will be excluded if and only if the test string matches exactly.
//				 Uses the Configuration classes' multi-value conventions, with
//				 multiple entires in one string separated by Configuration::MultiValueSeparator
//		Created: 28/1/04
//
// --------------------------------------------------------------------------
void ExcludeList::AddDefiniteEntries(const std::string &rEntries)
{
	// Split strings up
	std::vector<std::string> ens;
	SplitString(rEntries, Configuration::MultiValueSeparator, ens);
	
	// Add to set of excluded strings
	for(std::vector<std::string>::const_iterator i(ens.begin()); i != ens.end(); ++i)
	{
		if(i->size() > 0)
		{
			mDefinite.insert(*i);
		}
	}
}


// --------------------------------------------------------------------------
//
// Function
//		Name:    ExcludeList::AddRegexEntries(const std::string &)
//		Purpose: Adds a number of regular expression entries to the exclude list -- 
//				 if the test expression matches any of these regex, it will be excluded.
//				 Uses the Configuration classes' multi-value conventions, with
//				 multiple entires in one string separated by Configuration::MultiValueSeparator
//		Created: 28/1/04
//
// --------------------------------------------------------------------------
void ExcludeList::AddRegexEntries(const std::string &rEntries)
{
#ifdef WIN32
	//Under Win32 we use the boost library for the regular expression matching

	// Split strings up
	std::vector<std::string> ens;
	SplitString(rEntries, Configuration::MultiValueSeparator, ens);
	
	// Create and add new regular expressions
	for(std::vector<std::string>::const_iterator i(ens.begin()); i != ens.end(); ++i)
	{
		if(i->size() > 0)
		{
			try{
				boost::regex ourReg(i->c_str());
				this->mRegex.push_back(ourReg);
				// Store in list of regular expression string for Serialize
				this->mRegexStr.push_back(i->c_str());
			}
			catch(...)
			{
				THROW_EXCEPTION(CommonException, BadRegularExpression)
			}
		}
	}

#else // ! WIN32
#ifndef PLATFORM_REGEX_NOT_SUPPORTED

	// Split strings up
	std::vector<std::string> ens;
	SplitString(rEntries, Configuration::MultiValueSeparator, ens);
	
	// Create and add new regular expressions
	for(std::vector<std::string>::const_iterator i(ens.begin()); i != ens.end(); ++i)
	{
		if(i->size() > 0)
		{
			// Allocate memory
			regex_t *pregex = new regex_t;
			
			try
			{
				// Compile
				if(::regcomp(pregex, i->c_str(), REG_EXTENDED | REG_NOSUB) != 0)
				{
					THROW_EXCEPTION(CommonException, BadRegularExpression)
				}
				
				// Store in list of regular expressions
				mRegex.push_back(pregex);
			}
			catch(...)
			{
				delete pregex;
				throw;
			}
		}
	}

#else // PLATFORM_REGEX_NOT_SUPPORTED
	THROW_EXCEPTION(CommonException, RegexNotSupportedOnThisPlatform)
#endif // ! PLATFORM_REGEX_NOT_SUPPORTED
#endif // WIN32
}


// --------------------------------------------------------------------------
//
// Function
//		Name:    ExcludeList::IsExcluded(const std::string &)
//		Purpose: Returns true if the entry should be excluded
//		Created: 28/1/04
//
// --------------------------------------------------------------------------
bool ExcludeList::IsExcluded(const std::string &rTest) const
{
	// Check against the always include list
	if(mpAlwaysInclude != 0)
	{
		if(mpAlwaysInclude->IsExcluded(rTest))
		{
			// Because the "always include" list says it's 'excluded'
			// this means it should actually be included.
			return false;
		}
	}

	// Is it in the set of definite entries?
	if(mDefinite.find(rTest) != mDefinite.end())
	{
		return true;
	}
	
	// Check against regular expressions
#ifdef WIN32
	for(std::vector<boost::regex>::const_iterator i(mRegex.begin()); i != mRegex.end(); ++i)
	{
		// Test against this expression
		try
		{
		boost::smatch what;
		if(boost::regex_match(rTest, what, *i, boost::match_extra))
		{
			// match happened
			return true;
		}
		// In all other cases, including an error, just continue to the next expression
		}
		catch(...)
		{
			//just continue of no match
		}
	}
#else // ! WIN32
#ifndef PLATFORM_REGEX_NOT_SUPPORTED
	for(std::vector<regex_t *>::const_iterator i(mRegex.begin()); i != mRegex.end(); ++i)
	{
		// Test against this expression
		if(regexec(*i, rTest.c_str(), 0, 0 /* no match information required */, 0 /* no flags */) == 0)
		{
			// match happened
			return true;
		}
		// In all other cases, including an error, just continue to the next expression
	}
#endif // PLATFORM_REGEX_NOT_SUPPORTED
#endif // WIN32

	return false;
}


// --------------------------------------------------------------------------
//
// Function
//		Name:    ExcludeList::SetAlwaysIncludeList(ExcludeList *)
//		Purpose: Takes ownership of the list, deletes any pre-existing list.
//				 NULL is acceptable to delete the list.
//				 The AlwaysInclude list is a list of exceptions to the exclusions.
//		Created: 19/2/04
//
// --------------------------------------------------------------------------
void ExcludeList::SetAlwaysIncludeList(ExcludeList *pAlwaysInclude)
{
	// Delete old list
	if(mpAlwaysInclude != 0)
	{
		delete mpAlwaysInclude;
		mpAlwaysInclude = 0;
	}
	
	// Store the pointer
	mpAlwaysInclude = pAlwaysInclude;
}

