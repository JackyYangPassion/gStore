#pragma once
# #ifndef WEB_URL_H_

#endif // !WEB_URL_H_
#include <regex>
#include <string>
class WebUrl {
public:
	WebUrl(const string& url): _url(url){}
	WebUrl(string&& url) :_url(move(url)) {}

	string Request(const string& request)
	{
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