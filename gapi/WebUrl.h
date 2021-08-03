#pragma once
#ifndef WEB_URL_H_
#define WEB_URL_H)

#include <regex>
#include <string>
using namespace std;
class WebUrl {
public:
	WebUrl(const string& url): _url(url){}
	WebUrl(string&& url) :_url(move(url)) {}

	string Request(const string& request) const {
		smatch result;
		if (regex_search(_url.cbegin(), _url.cend(), result, regex(request + "=(.*?)&"))) {
			// ƥ����ж��������url

					// *? �ظ�����Σ������������ظ�  
			return result[1];

		}
		else if (regex_search(_url.cbegin(), _url.cend(), result, regex(request + "=(.*)"))) {
			// ƥ��ֻ��һ��������url

			return result[1];

		}
		else {
			// �����������ƶ�����������

			return string();

		}
	}
private:
	string _url;
};
#endif //WEB_URL_H_