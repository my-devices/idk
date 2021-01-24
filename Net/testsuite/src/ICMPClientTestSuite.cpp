//
// ICMPClientTestSuite.cpp
//
// Copyright (c) 2006, Applied Informatics Software Engineering GmbH.
// and Contributors.
//
// SPDX-License-Identifier:	BSL-1.0
//


#include "ICMPClientTestSuite.h"
#include "ICMPClientTest.h"
#include "ICMPSocketTest.h"


CppUnit::Test* ICMPClientTestSuite::suite()
{
	CppUnit::TestSuite* pSuite = new CppUnit::TestSuite("ICMPClientTestSuite");

	pSuite->addTest(ICMPClientTest::suite());
	pSuite->addTest(ICMPSocketTest::suite());

	return pSuite;
}
