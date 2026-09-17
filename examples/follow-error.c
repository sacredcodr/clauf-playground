int average(int total, int count)
{
    return total / count;
}

int main()
{
    // Integer division drops the fractional part.
    int total = 17;
    __clauf_print(average(total, 4));

    // What happens when there are no items?
    int count = 0;
    __clauf_print(average(total, count));

    return 0;
}
