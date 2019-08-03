// HTTPServer.cpp : Defines the entry point for the console application.
//
#include "webfront.h"
#include "page.h"
#include <iostream>



int main(int argc, char* argv[])
{
    {
        WebFront::Site my_site("1600");
        auto index_page = my_site.CreatePage("index.html");


        std::cout << "Press Return to quit" << std::endl; std::cin.get();
    }
    std::cout << "Press Return to quit" << std::endl; std::cin.get();
	return 0;
}

